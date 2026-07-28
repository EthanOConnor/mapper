/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_sync_controller.h"

#include <algorithm>
#include <limits>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <QXmlStreamReader>

#include "collaboration/map_hub_api_client.h"
#include "collaboration/map_hub_credentials.h"
#include "collaboration/map_hub_edit_transaction.h"
#include "collaboration/map_hub_entity_index.h"
#include "collaboration/map_hub_operation_store.h"
#include "collaboration/map_hub_sync_queue.h"
#include "core/document_path.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "core/symbols/symbol.h"
#include "undo/undo.h"
#include "undo/undo_manager.h"

namespace OpenOrienteering {

namespace {

constexpr auto snapshot_idle_ms = 10'000;
constexpr auto snapshot_max_ms = 60'000;
constexpr auto poll_active_ms = 30'000;

QString revisionId(const QJsonObject &object) {
  return object.value(QStringLiteral("id")).toString();
}

struct MutableObjectLocation {
  Object *object = nullptr;
  MapPart *part = nullptr;
  int part_index = -1;
  int object_index = -1;
};

MutableObjectLocation locateObject(Map &map, const QString &id) {
  for (int p = 0; p < map.getNumParts(); ++p) {
    auto *part = map.getPart(p);
    for (int o = 0; o < part->getNumObjects(); ++o) {
      auto *object = part->getObject(o);
      if (object->persistentId() == id)
        return {object, part, p, o};
    }
  }
  return {};
}

MapPart *locatePart(Map &map, const QString &id, int *part_index = nullptr) {
  for (int p = 0; p < map.getNumParts(); ++p) {
    auto *part = map.getPart(p);
    if (part->persistentId() == id) {
      if (part_index)
        *part_index = p;
      return part;
    }
  }
  return nullptr;
}

bool copyFileAtomically(const QString &source_path,
                        const QString &destination_path, qint64 expected_size) {
  QFile source(source_path);
  QSaveFile destination(destination_path);
  if (!source.open(QIODevice::ReadOnly) ||
      !destination.open(QIODevice::WriteOnly))
    return false;
  QByteArray buffer(1024 * 1024, Qt::Uninitialized);
  qint64 copied = 0;
  while (!source.atEnd()) {
    const auto read = source.read(buffer.data(), buffer.size());
    if (read <= 0 || destination.write(buffer.constData(), read) != read)
      return false;
    copied += read;
  }
  return copied == expected_size && destination.commit();
}

std::unique_ptr<Object> parseObjectFragment(Map &map, const QString &fragment,
                                            QString *error) {
  SymbolDictionary symbols;
  for (int i = 0; i < map.getNumSymbols(); ++i)
    symbols.insert(i, map.getSymbol(i));
  QXmlStreamReader xml(fragment);
  if (!xml.readNextStartElement() || xml.name() != QLatin1String("object")) {
    if (error)
      *error = QStringLiteral("Map Hub returned an invalid object fragment.");
    return {};
  }
  try {
    std::unique_ptr<Object> object(Object::load(xml, &map, symbols));
    if (xml.hasError() || xml.readNextStartElement()) {
      if (error)
        *error = QStringLiteral("Map Hub returned malformed object XML.");
      return {};
    }
    return object;
  } catch (const std::exception &exception) {
    if (error)
      *error = QString::fromUtf8(exception.what());
    return {};
  } catch (...) {
    if (error)
      *error = QStringLiteral("Map Hub returned an invalid object fragment.");
    return {};
  }
}

bool applyObjectTransactions(Map &map,
                             const QVector<MapHubEditTransaction> &transactions,
                             bool tolerate_already_applied, QString *error) {
  for (const auto &transaction : transactions) {
    for (const auto &operation : transaction.operations) {
      auto existing = locateObject(map, operation.object_id);
      if (operation.kind == MapHubEditOperation::Kind::DeleteObject) {
        if (!existing.object) {
          if (tolerate_already_applied)
            continue;
          if (error)
            *error =
                QStringLiteral("An upstream object was not present locally.");
          return false;
        }
        map.removeObjectFromSelection(existing.object, false);
        map.undoManager().adjustForExternalObjectChange(
            existing.part_index, existing.object_index, -1,
            operation.object_id);
        existing.part->deleteObject(existing.object_index);
        continue;
      }
      auto replacement = parseObjectFragment(map, operation.xml, error);
      if (!replacement || replacement->persistentId() != operation.object_id)
        return false;
      int target_part_index = -1;
      auto *target_part =
          locatePart(map, operation.part_id, &target_part_index);
      if (!target_part) {
        if (error)
          *error =
              QStringLiteral("A synchronized map part is missing locally.");
        return false;
      }
      if (existing.object) {
        map.removeObjectFromSelection(existing.object, false);
        map.undoManager().adjustForExternalObjectChange(
            existing.part_index, existing.object_index, -1,
            operation.object_id);
        delete existing.part->releaseObject(existing.object_index);
      }
      int position = 0;
      if (!operation.after_object_id.isEmpty()) {
        auto anchor = locateObject(map, operation.after_object_id);
        if (!anchor.object || anchor.part != target_part) {
          if (error)
            *error =
                QStringLiteral("A synchronized ordering anchor is missing.");
          return false;
        }
        position = anchor.object_index + 1;
      }
      map.undoManager().adjustForExternalObjectChange(
          target_part_index, position, 1, operation.object_id);
      target_part->addObject(replacement.release(), position);
    }
  }
  return true;
}

} // namespace

MapHubSyncController::MapHubSyncController(QObject *parent)
    : QObject(parent), watch_timer(new QTimer(this)),
      snapshot_idle_timer(new QTimer(this)),
      snapshot_max_timer(new QTimer(this)), upload_idle_timer(new QTimer(this)),
      retry_timer(new QTimer(this)), poll_timer(new QTimer(this)) {
  watch_timer->setInterval(1000);
  snapshot_idle_timer->setSingleShot(true);
  snapshot_max_timer->setSingleShot(true);
  upload_idle_timer->setSingleShot(true);
  retry_timer->setSingleShot(true);
  poll_timer->setInterval(poll_active_ms);
  connect(watch_timer, &QTimer::timeout, this,
          &MapHubSyncController::observeRevision);
  connect(snapshot_idle_timer, &QTimer::timeout, this,
          &MapHubSyncController::stageSnapshot);
  connect(snapshot_max_timer, &QTimer::timeout, this,
          &MapHubSyncController::stageSnapshot);
  connect(upload_idle_timer, &QTimer::timeout, this,
          &MapHubSyncController::drainOutbox);
  connect(retry_timer, &QTimer::timeout, this,
          &MapHubSyncController::retryWork);
  connect(poll_timer, &QTimer::timeout, this,
          &MapHubSyncController::pollSyncState);
}

MapHubSyncController::~MapHubSyncController() = default;

void MapHubSyncController::configure(const ManagedMapWorkspace &new_workspace,
                                     Map *new_map,
                                     RevisionProvider new_revision_provider,
                                     SnapshotProvider new_snapshot_provider) {
  clear();
  if (!new_workspace.isValid() || !new_map || !new_revision_provider ||
      !new_snapshot_provider)
    return;
  workspace = new_workspace;
  map = new_map;
  revision_provider = std::move(new_revision_provider);
  snapshot_provider = std::move(new_snapshot_provider);
  observed_revision = revision_provider();
  staged_revision = workspace.stream_protocol.isEmpty() ? 0 : observed_revision;
  operation_store = std::make_unique<MapHubOperationStore>();
  QString operation_error;
  if (!operation_store->open(workspace.workspace_id, &operation_error) ||
      (workspace.stream_protocol.isEmpty() &&
       !operation_store->seedInitialProjection(*map, &operation_error))) {
    operation_store.reset();
    setState(State::ActionRequired, operation_error);
    return;
  }
  const auto operation_state = operation_store->state(&operation_error);
  if (operation_state.client_instance_id.isEmpty()) {
    operation_store.reset();
    setState(State::ActionRequired,
             operation_error.isEmpty()
                 ? tr("Cannot initialize connected editing")
                 : operation_error);
    return;
  }
  workspace.client_instance_id = operation_state.client_instance_id;
  workspace.acknowledged_client_sequence =
      operation_state.acknowledged_client_sequence;
  const auto needs_projection_restore =
      workspace.stream_protocol == QLatin1String("oom-map-ops/1") &&
      operation_store->entityIndex(&operation_error).entities.isEmpty();
  if (workspace.stream_protocol == QLatin1String("oom-map-ops/1") &&
      !needs_projection_restore &&
      workspace.stream_head_sequence ==
          operation_state.published_stream_sequence &&
      workspace.stream_head_hash != operation_state.published_stream_hash) {
    setState(State::ActionRequired,
             tr("This map's connected-editing history did not verify"));
    return;
  }
  const auto recovered_transactions =
      operation_store->pendingTransactions(&operation_error);
  if (!operation_error.isEmpty() ||
      !applyObjectTransactions(*map, recovered_transactions, true,
                               &operation_error)) {
    stopped_for_conflict = true;
    workspace.sync_problem = QStringLiteral("local_recovery_failed");
    setState(State::ActionRequired,
             operation_error.isEmpty()
                 ? tr("Mapper could not restore locally saved Map Hub edits")
                 : operation_error);
    return;
  }
  if (!recovered_transactions.isEmpty()) {
    map->setObjectsDirty();
    map->requestRedraw();
    observed_revision = revision_provider();
    staged_revision = 0;
    stageSnapshot();
  }
  if (workspace.stream_protocol.isEmpty())
    stageSnapshot();
  saveWorkspace();
  edit_connection = connect(&map->undoManager(), &UndoManager::editCommitted,
                            this, &MapHubSyncController::queueCommittedEdit);
  if (needs_projection_restore) {
    stopped_for_conflict = true;
    setState(State::ActionRequired,
             tr("Restoring this map's connected-editing history…"));
  } else {
    setState(State::Watching, tr("Map Hub connected"));
  }
  watch_timer->start();
  poll_timer->start();
  QString queue_error;
  auto pending = MapHubSyncQueue::load(workspace.workspace_id, &queue_error);
  if (!needs_projection_restore && pending.isValid()) {
    staged_revision = pending.map_revision;
    setState(State::SavedLocally, tr("Saved locally — waiting to sync"));
  } else if (!queue_error.isEmpty()) {
    setState(State::ActionRequired,
             tr("Map Hub recovery queue needs attention"));
  }
  if (needs_projection_restore)
    QTimer::singleShot(0, this, &MapHubSyncController::restoreProjection);
  else
    QTimer::singleShot(0, this, &MapHubSyncController::pollSyncState);
  if (!needs_projection_restore && operation_store->pendingCount() > 0)
    upload_idle_timer->start(0);
}

void MapHubSyncController::clear() {
  for (auto *timer : {watch_timer, snapshot_idle_timer, snapshot_max_timer,
                      upload_idle_timer, retry_timer, poll_timer})
    timer->stop();
  if (request_client) {
    request_client->deleteLater();
    request_client = nullptr;
  }
  QObject::disconnect(edit_connection);
  edit_connection = {};
  operation_store.reset();
  map = nullptr;
  workspace = {};
  revision_provider = {};
  snapshot_provider = {};
  observed_revision = 0;
  staged_revision = 0;
  upload_pending = false;
  poll_pending = false;
  pull_pending = false;
  restore_pending = false;
  stopped_for_conflict = false;
  applying_remote_operations = false;
  setState(State::Disconnected, {});
}

bool MapHubSyncController::editable() const {
  return workspace.isValid() &&
         workspace.status != QLatin1String("submitted") &&
         workspace.status != QLatin1String("complete") &&
         workspace.status != QLatin1String("cancelled");
}

bool MapHubSyncController::checkpointStreamHead(
    qint64 *sequence, QString *hash, QString *entity_index_sha256) const {
  if (!operation_store || upload_pending || pull_pending ||
      stopped_for_conflict || operation_store->pendingCount() != 0)
    return false;
  const auto state = operation_store->state();
  if (workspace.stream_protocol != QLatin1String("oom-map-ops/1") ||
      state.published_stream_sequence != workspace.stream_head_sequence ||
      state.published_stream_hash != workspace.stream_head_hash)
    return false;
  if (sequence)
    *sequence = state.published_stream_sequence;
  if (hash)
    *hash = state.published_stream_hash;
  if (entity_index_sha256) {
    QString index_error;
    const auto digest =
        operation_store->entityIndex(&index_error).sha256(&index_error);
    if (digest.isEmpty())
      return false;
    *entity_index_sha256 = digest;
  }
  return true;
}

void MapHubSyncController::applicationBecameActive() {
  if (!workspace.isValid())
    return;
  watch_timer->start();
  poll_timer->start();
  pollSyncState();
  if (workspace.initial_snapshot_required || workspace.compaction_recommended ||
      workspace.compaction_required)
    uploadPendingSnapshot();
  else
    drainOutbox();
}

void MapHubSyncController::applicationWillResignActive() {
  if (!workspace.isValid())
    return;
  observeRevision();
  if (observed_revision != staged_revision)
    stageSnapshot();
}

void MapHubSyncController::savedExplicitly() {
  if (!workspace.isValid())
    return;
  observeRevision();
  scheduleSnapshot(true);
}

void MapHubSyncController::observeRevision() {
  if (!editable() || !revision_provider || stopped_for_conflict)
    return;
  const auto revision = revision_provider();
  if (!revision || revision == observed_revision)
    return;
  observed_revision = revision;
  scheduleSnapshot();
}

void MapHubSyncController::scheduleSnapshot(bool urgent) {
  if (!editable() || stopped_for_conflict)
    return;
  if (!snapshot_max_timer->isActive())
    snapshot_max_timer->start(snapshot_max_ms);
  snapshot_idle_timer->start(urgent ? 0 : snapshot_idle_ms);
}

void MapHubSyncController::stageSnapshot() {
  snapshot_idle_timer->stop();
  snapshot_max_timer->stop();
  if (!editable() || stopped_for_conflict || !snapshot_provider ||
      observed_revision == staged_revision)
    return;
  auto directory = MapHubSyncQueue::workspaceDirectory(workspace.workspace_id);
  if (!QDir().mkpath(directory)) {
    setState(State::ActionRequired,
             tr("Cannot create the Map Hub recovery folder"));
    return;
  }
  auto temporary_path = QDir(directory).filePath(
      QStringLiteral(".staged-%1.omap")
          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  setState(State::SavingLocally, tr("Saving locally…"));
  quint64 snapshot_revision = 0;
  QString snapshot_error;
  if (!snapshot_provider(temporary_path, &snapshot_revision, &snapshot_error)) {
    QFile::remove(temporary_path);
    // Editing operations temporarily prevent serialization. Retry without
    // surfacing a modal failure.
    snapshot_idle_timer->start(5000);
    setState(State::Watching, snapshot_error.isEmpty()
                                  ? tr("Local save will retry")
                                  : snapshot_error);
    return;
  }
  QString hash_error;
  auto sha256 =
      MapHubApiClient::sha256ForFile(temporary_path, &hash_error).toLower();
  auto final_path =
      MapHubSyncQueue::snapshotPath(workspace.workspace_id, sha256);
  if (sha256.isEmpty() || final_path.isEmpty()) {
    QFile::remove(temporary_path);
    setState(State::ActionRequired,
             hash_error.isEmpty()
                 ? tr("Could not verify the local recovery copy")
                 : hash_error);
    return;
  }
  if (QFileInfo::exists(final_path)) {
    QFile::remove(temporary_path);
  } else if (!QFile::rename(temporary_path, final_path)) {
    QFile::remove(temporary_path);
    setState(State::ActionRequired,
             tr("Could not commit the local Map Hub recovery copy"));
    return;
  }
  MapHubPendingDraft pending;
  pending.workspace_id = workspace.workspace_id;
  pending.local_map_path = workspace.local_map_path;
  pending.snapshot_path = final_path;
  pending.sha256 = sha256;
  pending.size_bytes = QFileInfo(final_path).size();
  pending.map_revision = snapshot_revision;
  pending.expected_workspace_revision_id =
      workspace.active_revision_id.isEmpty() ? workspace.base_revision_id
                                             : workspace.active_revision_id;
  pending.expected_project_revision_id = workspace.project_revision_id.isEmpty()
                                             ? workspace.base_revision_id
                                             : workspace.project_revision_id;
  pending.idempotency_key = MapHubSyncQueue::idempotencyKey(
      pending.workspace_id, pending.expected_workspace_revision_id,
      pending.sha256);
  pending.staged_at = QDateTime::currentDateTimeUtc();
  QString queue_error;
  if (operation_store && operation_store->pendingCount() == 0 &&
      (workspace.stream_protocol.isEmpty() ||
       workspace.compaction_recommended || workspace.compaction_required)) {
    const auto bootstrap = workspace.stream_protocol.isEmpty();
    const auto index = bootstrap ? MapHubEntityIndex::bootstrap(*map)
                                 : operation_store->entityIndex(&queue_error);
    const auto bytes = index.canonicalBytes(&queue_error);
    pending.entity_index_sha256 = index.sha256(&queue_error);
    pending.entity_index_path = MapHubSyncQueue::entityIndexPath(
        workspace.workspace_id, pending.entity_index_sha256);
    pending.base_stream_sequence = index.stream_sequence;
    pending.base_stream_hash = index.stream_hash;
    pending.client_instance_id = workspace.client_instance_id;
    pending.bootstrap = bootstrap;
    pending.publish_snapshot =
        !bytes.isEmpty() && !pending.entity_index_path.isEmpty();
    if (!pending.publish_snapshot) {
      setState(State::ActionRequired, queue_error);
      return;
    }
    QSaveFile index_file(pending.entity_index_path);
    if (!index_file.open(QIODevice::WriteOnly) ||
        index_file.write(bytes) != bytes.size() || !index_file.commit()) {
      setState(State::ActionRequired,
               index_file.errorString().isEmpty()
                   ? tr("Could not preserve the Map Hub entity index")
                   : index_file.errorString());
      return;
    }
    QFile::setPermissions(pending.entity_index_path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
  if (!MapHubSyncQueue::save(pending, &queue_error)) {
    setState(State::ActionRequired, queue_error);
    return;
  }
  // A connected workspace is an autosaved working copy. Keep the ordinary
  // .omap path crash-consistent with the content-addressed recovery snapshot;
  // immutable Map Hub history is still created only by an explicit checkpoint.
  if (!copyFileAtomically(pending.snapshot_path, workspace.local_map_path,
                          pending.size_bytes)) {
    setState(State::ActionRequired,
             tr("Your edit is preserved in Map Hub recovery storage, but "
                "Mapper could not update the working .omap file"));
    return;
  }
  if (!upload_pending)
    MapHubSyncQueue::pruneSnapshots(workspace.workspace_id,
                                    pending.snapshot_path, nullptr,
                                    pending.entity_index_path);
  staged_revision = snapshot_revision;
  setState(State::SavedLocally, tr("Saved locally"));
  if (pending.publish_snapshot)
    QTimer::singleShot(0, this, &MapHubSyncController::uploadPendingSnapshot);
  // If editing advanced while serialization ran, immediately begin the next
  // coalescing window. The just-staged bytes remain a valid recovery point.
  observed_revision = revision_provider ? revision_provider() : staged_revision;
  if (observed_revision != staged_revision)
    scheduleSnapshot();
}

void MapHubSyncController::queueCommittedEdit(const UndoStep *step) {
  if (!step || !map || !operation_store || applying_remote_operations ||
      !editable() || stopped_for_conflict)
    return;
  QString error;
  const auto state = operation_store->state(&error);
  const auto client_sequence = state.acknowledged_client_sequence +
                               operation_store->pendingCount(&error) + 1;
  auto transaction = MapHubEditTransaction::fromUndoStep(
      *map, *step, operation_store->entityVersions(&error),
      state.client_instance_id, client_sequence,
      state.optimistic_stream_sequence, state.optimistic_stream_hash,
      workspace.active_revision_id.isEmpty() ? workspace.base_revision_id
                                             : workspace.active_revision_id,
      workspace.project_revision_id.isEmpty() ? workspace.base_revision_id
                                              : workspace.project_revision_id,
      &error);
  if (!transaction.isValid(&error) ||
      !operation_store->enqueue(transaction, &error)) {
    stopped_for_conflict = true;
    workspace.sync_problem = QStringLiteral("local_operation_queue");
    saveWorkspace();
    setState(State::ActionRequired,
             error.isEmpty() ? tr("Could not preserve this edit for Map Hub")
                             : error);
    return;
  }
  setState(State::SavedLocally, tr("Saved locally"));
  upload_idle_timer->start(250);
}

void MapHubSyncController::drainOutbox() {
  if (!editable() || stopped_for_conflict || upload_pending || !operation_store)
    return;
  QString error;
  const auto pending = operation_store->nextPending(&error);
  if (!pending.isValid()) {
    if (!error.isEmpty())
      setState(State::ActionRequired, error);
    else
      setState(State::Synced, tr("Synced to Map Hub"));
    return;
  }
  if (workspace.stream_protocol != QLatin1String("oom-map-ops/1")) {
    setState(State::SavedLocally, tr("Saved locally — preparing Map Hub sync"));
    return;
  }
  const auto now = QDateTime::currentDateTimeUtc();
  if (pending.next_attempt_at.isValid() && pending.next_attempt_at > now) {
    retry_timer->start(
        std::max<qint64>(1, now.msecsTo(pending.next_attempt_at)));
    return;
  }
  auto account = MapHubCredentials::readToken(workspace.server_url);
  auto lease =
      MapHubCredentials::readToken(MapHubCredentials::workspaceLeaseKey(
          workspace.server_url, workspace.workspace_id));
  if (!account || account.token.isEmpty() || !lease || lease.token.isEmpty()) {
    setState(State::ActionRequired,
             tr("Reopen this Map Hub map to renew its connection"));
    return;
  }
  request_client =
      new MapHubApiClient(workspace.server_url, account.token, this);
  upload_pending = true;
  setState(State::Syncing, tr("Syncing to Map Hub…"));
  request_client->postWorkspaceTransaction(
      workspace.workspace_id, pending.canonical_json, lease.token,
      [this, pending](const QJsonObject &response,
                      const MapHubApiClient::Error &request_error) {
        upload_pending = false;
        if (request_client) {
          request_client->deleteLater();
          request_client = nullptr;
        }
        if (request_error) {
          if (request_error.code == QLatin1String("stream_advanced")) {
            setState(State::Syncing, tr("Applying upstream changes…"));
            QTimer::singleShot(0, this, &MapHubSyncController::pullOperations);
            return;
          }
          if (request_error.code == QLatin1String("compaction_required")) {
            workspace.compaction_required = true;
            workspace.compaction_recommended = true;
            workspace.sync_problem = request_error.code;
            saveWorkspace();
            setState(State::SavedLocally,
                     tr("Saved locally — compacting Map Hub history"));
            pollSyncState();
            return;
          }
          if (request_error.code == QLatin1String("entity_conflict") ||
              request_error.code ==
                  QLatin1String("project_revision_advanced") ||
              request_error.code == QLatin1String("stale_workspace_revision")) {
            stopped_for_conflict = true;
            workspace.sync_problem = request_error.code;
            saveWorkspace();
            setState(State::UpstreamChanged,
                     tr("Upstream changed — local work is preserved"));
            emit upstreamChangeDetected(request_error.message);
            pollSyncState();
            return;
          }
          if (request_error.code == QLatin1String("lease_required") ||
              request_error.code == QLatin1String("invalid_workspace_state") ||
              request_error.code == QLatin1String("unsupported_protocol")) {
            stopped_for_conflict = true;
            workspace.sync_problem = request_error.code;
            saveWorkspace();
            setState(State::ActionRequired, request_error.message);
            return;
          }
          const auto exponent = std::min(pending.attempt_count, 6);
          const auto base_seconds = std::min(300, 5 * (1 << exponent));
          const auto jitter = QRandomGenerator::global()->bounded(
              std::max(1, base_seconds / 3));
          const auto retry_at =
              QDateTime::currentDateTimeUtc().addSecs(base_seconds + jitter);
          QString store_error;
          if (!operation_store->recordFailure(
                  pending.client_sequence, request_error.code,
                  request_error.message, retry_at, &store_error)) {
            setState(State::ActionRequired, store_error);
            return;
          }
          retry_timer->start(std::max<qint64>(
              1, QDateTime::currentDateTimeUtc().msecsTo(retry_at)));
          setState(State::WaitingForNetwork,
                   tr("Saved locally — waiting for connection"));
          return;
        }
        const auto sequence =
            response.value(QStringLiteral("stream_sequence")).toInteger(-1);
        const auto hash =
            response.value(QStringLiteral("stream_hash")).toString();
        const auto acknowledged =
            response.value(QStringLiteral("acknowledged_client_sequence"))
                .toInteger(-1);
        QString store_error;
        if (acknowledged != pending.client_sequence ||
            !operation_store->acknowledge(pending.client_sequence, sequence,
                                          hash, &store_error)) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("invalid_acknowledgement");
          saveWorkspace();
          setState(State::ActionRequired,
                   store_error.isEmpty()
                       ? tr("Map Hub returned an invalid acknowledgement")
                       : store_error);
          return;
        }
        workspace.acknowledged_client_sequence = acknowledged;
        workspace.applied_stream_sequence = sequence;
        workspace.stream_head_sequence = sequence;
        workspace.stream_head_hash = hash;
        workspace.last_synced_at = QDateTime::currentDateTimeUtc();
        workspace.sync_problem.clear();
        saveWorkspace();
        auto lease =
            MapHubCredentials::readToken(MapHubCredentials::workspaceLeaseKey(
                workspace.server_url, workspace.workspace_id));
        auto account = MapHubCredentials::readToken(workspace.server_url);
        if (lease && !lease.token.isEmpty() && account &&
            !account.token.isEmpty()) {
          auto *ack_client =
              new MapHubApiClient(workspace.server_url, account.token, this);
          const auto state = operation_store->state();
          ack_client->acknowledgeWorkspaceOperations(
              workspace.workspace_id,
              {{QStringLiteral("protocol"), QStringLiteral("oom-map-ops/1")},
               {QStringLiteral("client_instance_id"), state.client_instance_id},
               {QStringLiteral("applied_stream_sequence"), sequence}},
              lease.token,
              [ack_client](const QJsonObject &,
                           const MapHubApiClient::Error &) {
                ack_client->deleteLater();
              });
        }
        QTimer::singleShot(0, this, &MapHubSyncController::drainOutbox);
      });
}

void MapHubSyncController::uploadPendingSnapshot() {
  if ((!workspace.initial_snapshot_required &&
       !workspace.compaction_recommended && !workspace.compaction_required) ||
      upload_pending || stopped_for_conflict || !editable())
    return;
  QString queue_error;
  const auto pending =
      MapHubSyncQueue::load(workspace.workspace_id, &queue_error);
  if (!pending.isValid() || !pending.publish_snapshot) {
    setState(State::ActionRequired,
             queue_error.isEmpty()
                 ? tr("Preparing the first connected-editing snapshot")
                 : queue_error);
    return;
  }
  QFile index_file(pending.entity_index_path);
  if (!index_file.open(QIODevice::ReadOnly) ||
      index_file.size() > 16 * 1024 * 1024) {
    setState(State::ActionRequired,
             tr("Could not read the Map Hub entity index"));
    return;
  }
  const auto index_bytes = index_file.readAll();
  if (QString::fromLatin1(
          QCryptographicHash::hash(index_bytes, QCryptographicHash::Sha256)
              .toHex()) != pending.entity_index_sha256) {
    setState(State::ActionRequired,
             tr("The saved Map Hub entity index did not verify"));
    return;
  }
  const auto account = MapHubCredentials::readToken(workspace.server_url);
  const auto lease =
      MapHubCredentials::readToken(MapHubCredentials::workspaceLeaseKey(
          workspace.server_url, workspace.workspace_id));
  if (!account || account.token.isEmpty() || !lease || lease.token.isEmpty()) {
    setState(State::ActionRequired,
             tr("Reopen this Map Hub map to renew its connection"));
    return;
  }
  request_client =
      new MapHubApiClient(workspace.server_url, account.token, this);
  upload_pending = true;
  setState(State::Syncing, pending.bootstrap
                               ? tr("Starting connected editing…")
                               : tr("Compacting Map Hub history…"));
  request_client->uploadWorkspaceSnapshot(
      workspace.workspace_id, pending.snapshot_path, index_bytes,
      pending.base_stream_sequence, pending.base_stream_hash, pending.sha256,
      pending.size_bytes, pending.expected_workspace_revision_id,
      pending.expected_project_revision_id, pending.client_instance_id,
      lease.token, pending.idempotency_key,
      [this, pending](const QJsonObject &response,
                      const MapHubApiClient::Error &error) {
        upload_pending = false;
        if (request_client) {
          request_client->deleteLater();
          request_client = nullptr;
        }
        if (error) {
          if (error.code == QLatin1String("stale_workspace_revision") ||
              error.code == QLatin1String("project_revision_advanced") ||
              error.code == QLatin1String("invalid_workspace_state") ||
              error.code == QLatin1String("lease_required")) {
            stopped_for_conflict = true;
            workspace.sync_problem = error.code;
            saveWorkspace();
            setState(State::ActionRequired, error.message);
            return;
          }
          setState(State::WaitingForNetwork,
                   tr("Saved locally — waiting for connection"));
          retry_timer->start(5000);
          return;
        }
        const auto snapshot =
            response.value(QStringLiteral("snapshot")).toObject();
        const auto response_index =
            snapshot.value(QStringLiteral("entity_index")).toObject();
        if (response.value(QStringLiteral("protocol")).toString() !=
                QLatin1String("oom-map-ops/1") ||
            snapshot.value(QStringLiteral("base_stream_sequence"))
                    .toInteger(-1) != pending.base_stream_sequence ||
            snapshot.value(QStringLiteral("base_stream_hash")).toString() !=
                pending.base_stream_hash ||
            snapshot.value(QStringLiteral("sha256")).toString() !=
                pending.sha256 ||
            response_index.value(QStringLiteral("sha256")).toString() !=
                pending.entity_index_sha256) {
          stopped_for_conflict = true;
          workspace.sync_problem =
              QStringLiteral("invalid_snapshot_acknowledgement");
          saveWorkspace();
          setState(State::ActionRequired,
                   tr("Map Hub returned an invalid snapshot acknowledgement"));
          return;
        }
        workspace.stream_protocol = QStringLiteral("oom-map-ops/1");
        workspace.initial_snapshot_required = false;
        workspace.compaction_recommended = false;
        workspace.compaction_required = false;
        workspace.uncompacted_operations = 0;
        workspace.stream_head_sequence = pending.base_stream_sequence;
        workspace.stream_head_hash = pending.base_stream_hash;
        workspace.snapshot_stream_sequence = pending.base_stream_sequence;
        workspace.snapshot_stream_hash = pending.base_stream_hash;
        workspace.snapshot_id = snapshot.value(QStringLiteral("id")).toString();
        workspace.snapshot_sha256 = pending.sha256;
        workspace.snapshot_size_bytes = pending.size_bytes;
        workspace.snapshot_download_url =
            snapshot.value(QStringLiteral("download_url")).toString();
        workspace.snapshot_revision_id =
            snapshot.value(QStringLiteral("revision_id")).toString();
        workspace.snapshot_entity_index_sha256 =
            response_index.value(QStringLiteral("sha256")).toString();
        workspace.snapshot_entity_index_download_url =
            response_index.value(QStringLiteral("download_url")).toString();
        workspace.last_synced_at = QDateTime::currentDateTimeUtc();
        workspace.sync_problem.clear();
        saveWorkspace();
        if (revision_provider && revision_provider() == pending.map_revision &&
            operation_store && operation_store->pendingCount() == 0) {
          if (copyFileAtomically(pending.snapshot_path,
                                 workspace.local_map_path, pending.size_bytes))
            MapHubSyncQueue::remove(workspace.workspace_id);
        }
        setState(State::Synced, tr("Connected editing ready"));
        QTimer::singleShot(0, this, &MapHubSyncController::drainOutbox);
      });
}

void MapHubSyncController::retryWork() {
  if (workspace.sync_problem == QLatin1String("snapshot_restore_required")) {
    restoreProjection();
  } else if (workspace.initial_snapshot_required ||
             workspace.compaction_recommended || workspace.compaction_required)
    uploadPendingSnapshot();
  else
    drainOutbox();
}

void MapHubSyncController::restoreProjection() {
  if (restore_pending || !operation_store || !map)
    return;
  QString file_error;
  const auto local_sha =
      MapHubApiClient::sha256ForFile(workspace.local_map_path, &file_error);
  if (workspace.snapshot_sha256.size() != 64 ||
      workspace.snapshot_entity_index_sha256.size() != 64 ||
      workspace.snapshot_entity_index_download_url.isEmpty() ||
      local_sha.compare(workspace.snapshot_sha256, Qt::CaseInsensitive) != 0) {
    workspace.sync_problem = QStringLiteral("snapshot_restore_required");
    saveWorkspace();
    setState(State::ActionRequired,
             tr("Reopen this assignment from Map Hub to restore its verified "
                "connected-editing snapshot"));
    return;
  }
  const auto account = MapHubCredentials::readToken(workspace.server_url);
  if (!account || account.token.isEmpty()) {
    setState(State::ActionRequired,
             tr("Reconnect your Map Hub account to restore this map"));
    return;
  }
  auto *client = new MapHubApiClient(workspace.server_url, account.token, this);
  workspace.sync_problem = QStringLiteral("snapshot_restore_required");
  saveWorkspace();
  restore_pending = true;
  client->workspaceEntityIndex(
      QUrl(workspace.snapshot_entity_index_download_url),
      [this, client](const QJsonObject &response,
                     const MapHubApiClient::Error &request_error) {
        restore_pending = false;
        client->deleteLater();
        if (request_error) {
          setState(State::WaitingForNetwork,
                   tr("Waiting to restore Map Hub history"));
          retry_timer->start(5000);
          return;
        }
        QString restore_error;
        const auto index =
            MapHubEntityIndex::fromJson(response, &restore_error);
        const auto bytes = index.canonicalBytes(&restore_error);
        const auto digest = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
                .toHex());
        if (bytes.isEmpty() ||
            digest != workspace.snapshot_entity_index_sha256 ||
            index.stream_sequence != workspace.snapshot_stream_sequence ||
            index.stream_hash != workspace.snapshot_stream_hash ||
            !operation_store->replaceProjection(index, &restore_error)) {
          workspace.sync_problem =
              QStringLiteral("invalid_snapshot_entity_index");
          saveWorkspace();
          setState(State::ActionRequired,
                   restore_error.isEmpty()
                       ? tr("The Map Hub recovery index did not verify")
                       : restore_error);
          return;
        }
        stopped_for_conflict = false;
        workspace.sync_problem.clear();
        workspace.applied_stream_sequence = index.stream_sequence;
        saveWorkspace();
        setState(State::Syncing, tr("Replaying recent Map Hub edits…"));
        QTimer::singleShot(0, this, &MapHubSyncController::pullOperations);
      });
}

void MapHubSyncController::pullOperations() {
  if (pull_pending || upload_pending || stopped_for_conflict ||
      !operation_store || !editable())
    return;
  QString store_error;
  const auto local_state = operation_store->state(&store_error);
  if (!store_error.isEmpty()) {
    setState(State::ActionRequired, store_error);
    return;
  }
  const auto account = MapHubCredentials::readToken(workspace.server_url);
  if (!account || account.token.isEmpty())
    return;
  auto *client = new MapHubApiClient(workspace.server_url, account.token, this);
  pull_pending = true;
  setState(State::Syncing, tr("Applying upstream changes…"));
  client->workspaceOperations(
      workspace.workspace_id, local_state.published_stream_sequence, 256,
      [this, client](const QJsonObject &response,
                     const MapHubApiClient::Error &error) {
        pull_pending = false;
        restore_pending = false;
        client->deleteLater();
        if (error) {
          if (error.code == QLatin1String("snapshot_required")) {
            stopped_for_conflict = true;
            workspace.sync_problem = error.code;
            saveWorkspace();
            setState(State::ActionRequired,
                     tr("A newer Map Hub snapshot must be restored"));
          } else {
            setState(State::WaitingForNetwork,
                     tr("Saved locally — waiting for connection"));
          }
          return;
        }
        if (response.value(QStringLiteral("protocol")).toString() !=
            QLatin1String("oom-map-ops/1")) {
          setState(State::ActionRequired,
                   tr("Map Hub returned an unsupported operation stream"));
          return;
        }
        QVector<MapHubCommittedTransaction> transactions;
        QString transaction_error;
        for (const auto &value :
             response.value(QStringLiteral("transactions")).toArray()) {
          if (!value.isObject()) {
            transaction_error =
                QStringLiteral("Map Hub returned an invalid transaction.");
            break;
          }
          auto committed = MapHubCommittedTransaction::fromJson(
              value.toObject(), &transaction_error);
          if (!committed.isValid(&transaction_error))
            break;
          transactions.push_back(std::move(committed));
        }
        if (!transaction_error.isEmpty()) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("invalid_operation_page");
          saveWorkspace();
          setState(State::ActionRequired, transaction_error);
          return;
        }
        if (transactions.isEmpty()) {
          setState(State::Synced, tr("Synced to Map Hub"));
          QTimer::singleShot(0, this, &MapHubSyncController::drainOutbox);
          return;
        }
        if (!operation_store->rebaseOnto(transactions, &transaction_error)) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("entity_conflict");
          saveWorkspace();
          setState(State::UpstreamChanged,
                   tr("Upstream and local edits need review"));
          emit upstreamChangeDetected(transaction_error);
          return;
        }

        applying_remote_operations = true;
        for (const auto &committed : transactions) {
          for (const auto &operation : committed.transaction.operations) {
            auto existing = locateObject(*map, operation.object_id);
            if (operation.kind == MapHubEditOperation::Kind::DeleteObject) {
              if (!existing.object) {
                transaction_error = QStringLiteral(
                    "An upstream object was not present locally.");
                break;
              }
              map->removeObjectFromSelection(existing.object, false);
              map->undoManager().adjustForExternalObjectChange(
                  existing.part_index, existing.object_index, -1,
                  operation.object_id);
              existing.part->deleteObject(existing.object_index);
              continue;
            }
            auto replacement =
                parseObjectFragment(*map, operation.xml, &transaction_error);
            if (!replacement ||
                replacement->persistentId() != operation.object_id)
              break;
            int target_part_index = -1;
            auto *target_part =
                locatePart(*map, operation.part_id, &target_part_index);
            if (!target_part) {
              transaction_error =
                  QStringLiteral("An upstream map part is missing locally.");
              break;
            }
            if (existing.object) {
              map->removeObjectFromSelection(existing.object, false);
              map->undoManager().adjustForExternalObjectChange(
                  existing.part_index, existing.object_index, -1,
                  operation.object_id);
              delete existing.part->releaseObject(existing.object_index);
            }
            int position = 0;
            if (!operation.after_object_id.isEmpty()) {
              auto anchor = locateObject(*map, operation.after_object_id);
              if (!anchor.object || anchor.part != target_part) {
                transaction_error =
                    QStringLiteral("An upstream ordering anchor is missing.");
                break;
              }
              position = anchor.object_index + 1;
            }
            map->undoManager().adjustForExternalObjectChange(
                target_part_index, position, 1, operation.object_id);
            target_part->addObject(replacement.release(), position);
          }
          if (!transaction_error.isEmpty())
            break;
        }
        applying_remote_operations = false;
        if (!transaction_error.isEmpty()) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("local_projection_mismatch");
          saveWorkspace();
          setState(State::ActionRequired, transaction_error);
          return;
        }
        map->setObjectsDirty();
        map->requestRedraw();
        const auto &last = transactions.constLast();
        workspace.applied_stream_sequence = last.stream_sequence;
        workspace.stream_head_sequence = last.stream_sequence;
        workspace.stream_head_hash = last.stream_hash;
        workspace.last_synced_at = QDateTime::currentDateTimeUtc();
        workspace.sync_problem.clear();
        saveWorkspace();

        const auto lease =
            MapHubCredentials::readToken(MapHubCredentials::workspaceLeaseKey(
                workspace.server_url, workspace.workspace_id));
        const auto state = operation_store->state();
        const auto ack_account =
            MapHubCredentials::readToken(workspace.server_url);
        if (lease && !lease.token.isEmpty() && ack_account &&
            !ack_account.token.isEmpty()) {
          auto *ack_client = new MapHubApiClient(workspace.server_url,
                                                 ack_account.token, this);
          ack_client->acknowledgeWorkspaceOperations(
              workspace.workspace_id,
              {{QStringLiteral("protocol"), QStringLiteral("oom-map-ops/1")},
               {QStringLiteral("client_instance_id"), state.client_instance_id},
               {QStringLiteral("applied_stream_sequence"),
                workspace.applied_stream_sequence}},
              lease.token,
              [ack_client](const QJsonObject &,
                           const MapHubApiClient::Error &) {
                ack_client->deleteLater();
              });
        }
        if (response.value(QStringLiteral("has_more")).toBool())
          QTimer::singleShot(0, this, &MapHubSyncController::pullOperations);
        else
          QTimer::singleShot(0, this, &MapHubSyncController::drainOutbox);
      });
}

void MapHubSyncController::pollSyncState() {
  if (!workspace.isValid() || poll_pending)
    return;
  auto account = MapHubCredentials::readToken(workspace.server_url);
  if (!account || account.token.isEmpty())
    return;
  auto *client = new MapHubApiClient(workspace.server_url, account.token, this);
  poll_pending = true;
  client->workspaceSyncState(
      workspace.workspace_id, workspace.sync_etag,
      [this, client](const QJsonObject &response, const QString &etag,
                     bool not_modified,
                     const MapHubApiClient::Error &error) mutable {
        poll_pending = false;
        client->deleteLater();
        if (error || not_modified)
          return;
        if (!etag.isEmpty())
          workspace.sync_etag = etag;
        const auto workspace_object =
            response.value(QStringLiteral("workspace")).toObject();
        const auto workspace_revision =
            response.value(QStringLiteral("workspace_revision")).toObject();
        const auto project_revision =
            response.value(QStringLiteral("project_revision")).toObject();
        const auto stream = response.value(QStringLiteral("stream")).toObject();
        if (!stream.isEmpty()) {
          const auto protocol =
              stream.value(QStringLiteral("protocol")).toString();
          const auto head_sequence =
              stream.value(QStringLiteral("head_sequence")).toInteger(-1);
          const auto head_hash =
              stream.value(QStringLiteral("head_hash")).toString();
          const auto minimum_sequence =
              stream.value(QStringLiteral("minimum_available_sequence"))
                  .toInteger(-1);
          if (protocol == QLatin1String("oom-map-ops/1") &&
              head_sequence >= 0 && head_hash.size() == 64 &&
              minimum_sequence >= 0 &&
              (minimum_sequence <= head_sequence ||
               (head_sequence < std::numeric_limits<qint64>::max() &&
                minimum_sequence == head_sequence + 1))) {
            workspace.stream_protocol = protocol;
            workspace.stream_head_sequence = head_sequence;
            workspace.stream_head_hash = head_hash;
            workspace.minimum_available_sequence = minimum_sequence;
            workspace.initial_snapshot_required =
                stream.value(QStringLiteral("initial_snapshot_required"))
                    .toBool();
            workspace.uncompacted_operations =
                stream.value(QStringLiteral("uncompacted_operations"))
                    .toInteger(0);
            workspace.compaction_recommended =
                stream.value(QStringLiteral("compaction_recommended")).toBool();
            workspace.compaction_required =
                stream.value(QStringLiteral("compaction_required")).toBool();
            const auto snapshot =
                stream.value(QStringLiteral("snapshot")).toObject();
            if (!snapshot.isEmpty()) {
              workspace.snapshot_stream_sequence =
                  snapshot.value(QStringLiteral("base_stream_sequence"))
                      .toInteger(0);
              workspace.snapshot_stream_hash =
                  snapshot.value(QStringLiteral("base_stream_hash")).toString();
              workspace.snapshot_id =
                  snapshot.value(QStringLiteral("id")).toString();
              workspace.snapshot_sha256 =
                  snapshot.value(QStringLiteral("sha256")).toString();
              workspace.snapshot_size_bytes =
                  snapshot.value(QStringLiteral("size_bytes")).toInteger();
              workspace.snapshot_download_url =
                  snapshot.value(QStringLiteral("download_url")).toString();
              workspace.snapshot_revision_id =
                  snapshot.value(QStringLiteral("revision_id")).toString();
              const auto entity_index =
                  snapshot.value(QStringLiteral("entity_index")).toObject();
              workspace.snapshot_entity_index_sha256 =
                  entity_index.value(QStringLiteral("sha256")).toString();
              workspace.snapshot_entity_index_download_url =
                  entity_index.value(QStringLiteral("download_url")).toString();
            }
          }
        }
        const auto remote_status =
            workspace_object.value(QStringLiteral("status")).toString();
        const auto remote_workspace_revision = revisionId(workspace_revision);
        const auto known_workspace_revision =
            workspace.active_revision_id.isEmpty()
                ? workspace.base_revision_id
                : workspace.active_revision_id;
        const auto remote_project_revision = revisionId(project_revision);
        const auto known_project_revision =
            workspace.project_revision_id.isEmpty()
                ? workspace.base_revision_id
                : workspace.project_revision_id;
        if (!remote_status.isEmpty())
          workspace.status = remote_status;
        if (workspace.active_revision_id.isEmpty() &&
            !remote_workspace_revision.isEmpty())
          workspace.active_revision_id = remote_workspace_revision;
        if (workspace.project_revision_id.isEmpty() &&
            !remote_project_revision.isEmpty())
          workspace.project_revision_id = remote_project_revision;
        if (!remote_workspace_revision.isEmpty() &&
            !known_workspace_revision.isEmpty() &&
            remote_workspace_revision != known_workspace_revision) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("workspace_advanced");
          setState(State::UpstreamChanged,
                   tr("This workspace changed on another device"));
          emit upstreamChangeDetected(
              tr("Map Hub has a newer workspace revision. Your local map and "
                 "pending edits were preserved."));
        } else if (!remote_project_revision.isEmpty() &&
                   !known_project_revision.isEmpty() &&
                   remote_project_revision != known_project_revision) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("project_advanced");
          setState(State::UpstreamChanged,
                   tr("A newer approved map is available"));
          emit upstreamChangeDetected(
              tr("The approved Map Hub map changed upstream. Your local map "
                 "and pending edits were preserved."));
        }
        saveWorkspace();
        if ((workspace.compaction_recommended ||
             workspace.compaction_required) &&
            operation_store && operation_store->pendingCount() == 0) {
          staged_revision = 0;
          scheduleSnapshot(true);
        } else if (workspace.initial_snapshot_required)
          QTimer::singleShot(0, this,
                             &MapHubSyncController::uploadPendingSnapshot);
        else if (operation_store &&
                 workspace.stream_head_sequence >
                     operation_store->state().published_stream_sequence)
          QTimer::singleShot(0, this, &MapHubSyncController::pullOperations);
        else
          QTimer::singleShot(0, this, &MapHubSyncController::drainOutbox);
      });
}

void MapHubSyncController::setState(State state, const QString &text) {
  if (current_state == state && state_text == text)
    return;
  current_state = state;
  state_text = text;
  emit stateChanged(state, text);
}

bool MapHubSyncController::saveWorkspace() {
  QString error;
  if (ManagedMapWorkspace::save(workspace, &error))
    return true;
  setState(State::ActionRequired,
           tr("Could not update Map Hub sync metadata: %1").arg(error));
  return false;
}

} // namespace OpenOrienteering
