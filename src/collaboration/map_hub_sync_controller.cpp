/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_sync_controller.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
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
#include "fileformats/xml_file_format.h"
#include "fileformats/xml_file_format_p.h"
#include "undo/undo.h"
#include "undo/undo_manager.h"

namespace OpenOrienteering {

namespace {

constexpr auto snapshot_idle_ms = 10'000;
constexpr auto snapshot_max_ms = 60'000;
constexpr auto poll_active_ms = 30'000;
constexpr auto poll_active_min_ms = 750;
constexpr auto poll_active_max_ms = 60'000;
constexpr auto poll_idle_min_ms = 2'000;
constexpr auto poll_idle_max_ms = 300'000;

bool validStreamHash(const QString &value) {
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return pattern.match(value).hasMatch();
}

bool retryableRequestError(const MapHubApiClient::Error &error) {
  return error.http_status == 0 || error.http_status == 408 ||
         error.http_status == 429 || error.http_status >= 500;
}

bool isUpstreamProblem(const QString &code) {
  return code == QLatin1String("entity_conflict") ||
         code == QLatin1String("invalid_anchor") ||
         code == QLatin1String("project_revision_advanced") ||
         code == QLatin1String("stale_workspace_revision") ||
         code == QLatin1String("workspace_advanced") ||
         code == QLatin1String("project_advanced");
}

bool isTerminalWorkspaceStatus(const QString &status) {
  return status == QLatin1String("submitted") ||
         status == QLatin1String("complete") ||
         status == QLatin1String("cancelled");
}

QString presenceDeviceName() {
#if defined(Q_OS_IOS)
  return QStringLiteral("Mapper on iOS");
#elif defined(Q_OS_ANDROID)
  return QStringLiteral("Mapper on Android");
#elif defined(Q_OS_MACOS)
  return QStringLiteral("Mapper on macOS");
#elif defined(Q_OS_WIN)
  return QStringLiteral("Mapper on Windows");
#else
  return QStringLiteral("Mapper on %1").arg(QSysInfo::productType());
#endif
}

QString revisionId(const QJsonObject &object) {
  return object.value(QStringLiteral("id")).toString();
}

struct MutableObjectLocation {
  Object *object = nullptr;
  MapPart *part = nullptr;
  int part_index = -1;
  int object_index = -1;
};

struct MutableSymbolLocation {
  Symbol *symbol = nullptr;
  int index = -1;
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

MutableSymbolLocation locateSymbol(Map &map, const QString &id) {
  for (int i = 0; i < map.getNumSymbols(); ++i) {
    auto *symbol = map.getSymbol(i);
    if (symbol->persistentId() == id)
      return {symbol, i};
  }
  return {};
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

bool commitContentAddressedFile(const QString &temporary_path,
                                const QString &final_path,
                                const QString &expected_sha256,
                                qint64 expected_size) {
  if (QFileInfo::exists(final_path)) {
    QString hash_error;
    const auto existing_sha =
        MapHubApiClient::sha256ForFile(final_path, &hash_error);
    if (existing_sha == expected_sha256 &&
        QFileInfo(final_path).size() == expected_size)
      return QFile::remove(temporary_path);
    if (!QFile::remove(final_path))
      return false;
  }
  return QFile::rename(temporary_path, final_path);
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

std::unique_ptr<Symbol> parseSymbolFragment(Map &map, const QString &fragment,
                                            QString *error) {
  SymbolDictionary symbols;
  QXmlStreamReader xml(fragment);
  if (!xml.readNextStartElement() || xml.name() != QLatin1String("symbol")) {
    if (error)
      *error = QStringLiteral("Map Hub returned an invalid symbol fragment.");
    return {};
  }
  try {
    auto symbol =
        Symbol::load(xml, map, symbols, XMLFileFormat::current_version);
    if (xml.hasError() || xml.readNextStartElement()) {
      if (error)
        *error = QStringLiteral("Map Hub returned malformed symbol XML.");
      return {};
    }
    return symbol;
  } catch (const std::exception &exception) {
    if (error)
      *error = QString::fromUtf8(exception.what());
    return {};
  } catch (...) {
    if (error)
      *error = QStringLiteral("Map Hub returned an invalid symbol fragment.");
    return {};
  }
}

bool applyMapTransactions(Map &map,
                          const QVector<MapHubEditTransaction> &transactions,
                          bool tolerate_already_applied, QString *error) {
  for (const auto &transaction : transactions) {
    for (const auto &operation : transaction.operations) {
      if (operation.kind == MapHubEditOperation::Kind::DeletePart) {
        int part_index = -1;
        const auto *part = locatePart(map, operation.entity_id, &part_index);
        if (!part) {
          if (tolerate_already_applied)
            continue;
          if (error)
            *error =
                QStringLiteral("An upstream map part was not present locally.");
          return false;
        }
        if (part->getNumObjects() != 0 || map.getNumParts() <= 1) {
          if (error)
            *error = QStringLiteral(
                "An upstream map part could not be removed safely.");
          return false;
        }
        map.removePart(std::size_t(part_index));
        continue;
      }
      if (operation.kind == MapHubEditOperation::Kind::PutPart) {
        int old_index = -1;
        auto *part = locatePart(map, operation.entity_id, &old_index);
        int desired_index = 0;
        if (!operation.after_id.isEmpty()) {
          int anchor_index = -1;
          const auto *anchor =
              locatePart(map, operation.after_id, &anchor_index);
          if (!anchor) {
            if (error)
              *error = QStringLiteral("An upstream part anchor is missing.");
            return false;
          }
          if (part && anchor_index > old_index)
            --anchor_index;
          desired_index = anchor_index + 1;
        }
        if (!part) {
          part = new MapPart(operation.payload, &map, operation.entity_id);
          map.addPart(part, std::size_t(desired_index));
        } else {
          if (part->getName() != operation.payload)
            part->setName(operation.payload);
          if (old_index != desired_index)
            map.movePart(std::size_t(old_index), std::size_t(desired_index));
        }
        continue;
      }
      if (operation.kind == MapHubEditOperation::Kind::DeleteSymbol) {
        const auto existing = locateSymbol(map, operation.entity_id);
        if (!existing.symbol) {
          if (tolerate_already_applied)
            continue;
          if (error)
            *error =
                QStringLiteral("An upstream symbol was not present locally.");
          return false;
        }
        if (map.existsObjectWithSymbol(existing.symbol)) {
          if (error)
            *error = QStringLiteral(
                "An upstream symbol is still used by a local object.");
          return false;
        }
        map.deleteSymbol(existing.index);
        continue;
      }
      if (operation.kind == MapHubEditOperation::Kind::PutSymbol) {
        auto replacement = parseSymbolFragment(map, operation.payload, error);
        if (!replacement || replacement->persistentId() != operation.entity_id)
          return false;
        auto existing = locateSymbol(map, operation.entity_id);
        int desired_index = 0;
        if (!operation.after_id.isEmpty()) {
          auto anchor = locateSymbol(map, operation.after_id);
          if (!anchor.symbol) {
            if (error)
              *error = QStringLiteral("An upstream symbol anchor is missing.");
            return false;
          }
          if (existing.symbol && anchor.index > existing.index)
            --anchor.index;
          desired_index = anchor.index + 1;
        }
        Symbol *installed = replacement.get();
        if (!existing.symbol) {
          map.addSymbol(replacement.release(), desired_index);
        } else {
          map.setSymbol(replacement.release(), existing.index);
          if (existing.index != desired_index) {
            const auto insertion_index = existing.index < desired_index
                                             ? desired_index + 1
                                             : desired_index;
            map.moveSymbol(existing.index, insertion_index);
          }
        }
        if (!installed->loadingFinishedEvent(&map)) {
          if (error)
            *error =
                QStringLiteral("An upstream symbol could not be resolved.");
          return false;
        }
        continue;
      }

      auto existing = locateObject(map, operation.entity_id);
      if (operation.kind == MapHubEditOperation::Kind::DeleteObject) {
        if (!existing.object) {
          if (tolerate_already_applied)
            continue;
          if (error)
            *error =
                QStringLiteral("An upstream object was not present locally.");
          return false;
        }
        if (map.isObjectSelected(existing.object))
          map.removeObjectFromSelection(existing.object, false);
        map.undoManager().adjustForExternalObjectChange(
            existing.part_index, existing.object_index, -1,
            operation.entity_id);
        existing.part->deleteObject(existing.object_index);
        continue;
      }
      auto replacement = parseObjectFragment(map, operation.payload, error);
      if (!replacement || replacement->persistentId() != operation.entity_id)
        return false;
      int target_part_index = -1;
      auto *target_part =
          locatePart(map, operation.parent_id, &target_part_index);
      if (!target_part) {
        if (error)
          *error =
              QStringLiteral("A synchronized map part is missing locally.");
        return false;
      }
      if (existing.object) {
        if (map.isObjectSelected(existing.object))
          map.removeObjectFromSelection(existing.object, false);
        map.undoManager().adjustForExternalObjectChange(
            existing.part_index, existing.object_index, -1,
            operation.entity_id);
        delete existing.part->releaseObject(existing.object_index);
      }
      int position = 0;
      if (!operation.after_id.isEmpty()) {
        auto anchor = locateObject(map, operation.after_id);
        if (!anchor.object || anchor.part != target_part) {
          if (error)
            *error =
                QStringLiteral("A synchronized ordering anchor is missing.");
          return false;
        }
        position = anchor.object_index + 1;
      }
      map.undoManager().adjustForExternalObjectChange(
          target_part_index, position, 1, operation.entity_id);
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
      retry_timer(new QTimer(this)), poll_timer(new QTimer(this)),
      presence_expiry_timer(new QTimer(this)) {
  watch_timer->setInterval(1000);
  snapshot_idle_timer->setSingleShot(true);
  snapshot_max_timer->setSingleShot(true);
  upload_idle_timer->setSingleShot(true);
  retry_timer->setSingleShot(true);
  presence_expiry_timer->setSingleShot(true);
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
  connect(presence_expiry_timer, &QTimer::timeout, this, [this] {
    emit detailsChanged();
    schedulePresenceExpiry();
  });
}

MapHubSyncController::~MapHubSyncController() = default;

void MapHubSyncController::configure(const ManagedMapWorkspace &new_workspace,
                                     Map *new_map,
                                     RevisionProvider new_revision_provider,
                                     SnapshotProvider new_snapshot_provider,
                                     WorkingCopyCommitter
                                         new_working_copy_committer) {
  const auto refreshing_same_session =
      workspace.isValid() && new_workspace.isValid() && map == new_map &&
      MapHubApiClient::canonicalServerOrigin(workspace.server_url) ==
          MapHubApiClient::canonicalServerOrigin(new_workspace.server_url) &&
      workspace.workspace_id == new_workspace.workspace_id;
  clearSession(!refreshing_same_session);
  if (!new_workspace.isValid() || !new_map || !new_revision_provider ||
      !new_snapshot_provider)
    return;
  workspace = new_workspace;
  if (workspace.sync_problem == QLatin1String("workspace_metadata")) {
    workspace_metadata_pending = true;
    workspace_metadata_desired_stopped = false;
    workspace_metadata_desired_problem.clear();
  }
  map = new_map;
  revision_provider = std::move(new_revision_provider);
  snapshot_provider = std::move(new_snapshot_provider);
  working_copy_committer = std::move(new_working_copy_committer);
  application_active = true;
  active_poll_interval_ms = poll_active_ms;
  idle_poll_interval_ms = 120'000;
  poll_timer->setInterval(active_poll_interval_ms);
  observed_revision = revision_provider();
  operation_store = std::make_unique<MapHubOperationStore>();
  QString operation_error;
  const auto needs_initial_seed =
      workspace.stream_protocol.isEmpty() ||
      (workspace.stream_protocol == QLatin1String("oom-map-ops/1") &&
       workspace.initial_snapshot_required &&
       workspace.stream_head_sequence == 0);
  staged_revision = needs_initial_seed ? 0 : observed_revision;
  if (!operation_store->open(workspace.workspace_id,
                             workspace.client_instance_id,
                             &operation_error) ||
      (needs_initial_seed &&
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
  const auto projection_empty =
      operation_store->entityIndex(&operation_error).entities.isEmpty();
  QString local_hash_error;
  const auto local_is_stream_snapshot =
      !workspace.snapshot_sha256.isEmpty() &&
      MapHubApiClient::sha256ForFile(workspace.local_map_path,
                                     &local_hash_error)
              .compare(workspace.snapshot_sha256, Qt::CaseInsensitive) == 0;
  const auto needs_projection_restore =
      workspace.stream_protocol == QLatin1String("oom-map-ops/1") &&
      !workspace.initial_snapshot_required &&
      (projection_empty || (local_is_stream_snapshot &&
                            (operation_state.published_stream_sequence !=
                                 workspace.snapshot_stream_sequence ||
                             operation_state.published_stream_hash !=
                                 workspace.snapshot_stream_hash)));
  if (!operation_error.isEmpty()) {
    setState(State::ActionRequired, operation_error);
    return;
  }
  if (workspace.stream_protocol == QLatin1String("oom-map-ops/1") &&
      !needs_projection_restore &&
      workspace.stream_head_sequence ==
          operation_state.published_stream_sequence &&
      workspace.stream_head_hash != operation_state.published_stream_hash) {
    setState(State::ActionRequired,
             tr("This map's connected-editing history did not verify"));
    return;
  }
  const auto unapplied_remote =
      needs_projection_restore
          ? QVector<MapHubCommittedTransaction>{}
          : operation_store->unappliedTransactions(&operation_error);
  QVector<MapHubEditTransaction> unapplied_remote_transactions;
  for (const auto &committed : unapplied_remote)
    unapplied_remote_transactions.push_back(committed.transaction);
  if (!operation_error.isEmpty() ||
      !applyMapTransactions(*map, unapplied_remote_transactions, true,
                            &operation_error)) {
    stopped_for_conflict = true;
    workspace.sync_problem = QStringLiteral("local_recovery_failed");
    setState(State::ActionRequired,
             operation_error.isEmpty()
                 ? tr("Mapper could not finish applying saved Map Hub edits")
                 : operation_error);
    return;
  }
  const auto recovered_transactions =
      needs_projection_restore
          ? QVector<MapHubEditTransaction>{}
          : operation_store->pendingTransactions(&operation_error);
  if (!operation_error.isEmpty() ||
      !applyMapTransactions(*map, recovered_transactions, true,
                            &operation_error)) {
    stopped_for_conflict = true;
    workspace.sync_problem = QStringLiteral("local_recovery_failed");
    setState(State::ActionRequired,
             operation_error.isEmpty()
                 ? tr("Mapper could not restore locally saved Map Hub edits")
                 : operation_error);
    return;
  }
  auto recovery_snapshot_ok = true;
  if (!unapplied_remote.isEmpty() || !recovered_transactions.isEmpty()) {
    map->setObjectsDirty();
    map->requestRedraw();
    observed_revision = revision_provider();
    staged_revision = 0;
    recovery_snapshot_ok = stageSnapshotNow();
  }
  local_entities = captureLocalEntities();
  semantic_revision = revision_provider();
  if (needs_initial_seed)
    stageSnapshot();
  saveWorkspace();
  edit_connection = connect(map, &Map::editCommitted,
                            this, &MapHubSyncController::queueCommittedEdit);
  const auto structure_changed = [this] {
    if (!applying_remote_operations)
      scheduleStructureChanges();
  };
  const auto checkpoint_changed = [this] {
    if (!applying_remote_operations)
      markCheckpointRequired();
  };
  entity_connections = {
      connect(map, &Map::mapPartAdded, this, structure_changed),
      connect(map, &Map::mapPartChanged, this, structure_changed),
      connect(map, &Map::mapPartDeleted, this, structure_changed),
      connect(map, &Map::symbolAdded, this, structure_changed),
      connect(map, &Map::symbolChanged, this, structure_changed),
      connect(map, &Map::symbolDeleted, this, structure_changed),
      connect(map, &Map::symbolMoved, this, structure_changed),
      connect(map, &Map::colorAdded, this, checkpoint_changed),
      connect(map, &Map::colorChanged, this, checkpoint_changed),
      connect(map, &Map::colorDeleted, this, checkpoint_changed),
      connect(map, &Map::templateAdded, this, checkpoint_changed),
      connect(map, &Map::templateChanged, this, checkpoint_changed),
      connect(map, &Map::templateMoved, this, checkpoint_changed),
      connect(map, &Map::templateDeleted, this, checkpoint_changed),
      connect(map, &Map::firstFrontTemplateChanged, this,
              checkpoint_changed),
  };
  if (needs_projection_restore) {
    stopped_for_conflict = true;
    setState(State::ActionRequired,
             tr("Restoring this map's connected-editing history…"));
  } else if (!workspace.sync_problem.isEmpty() &&
             workspace.sync_problem != QLatin1String("compaction_required")) {
    stopped_for_conflict = true;
    setState(isUpstreamProblem(workspace.sync_problem)
                 ? State::UpstreamChanged
                 : State::ActionRequired,
             isUpstreamProblem(workspace.sync_problem)
                 ? tr("Upstream and local edits still need review")
                 : tr("Map Hub synchronization still needs attention"));
  } else if (recovery_snapshot_ok || current_state != State::ActionRequired) {
    setState(State::Watching, tr("Map Hub connected"));
  }
  watch_timer->start();
  poll_timer->start();
  QString queue_error;
  auto pending = MapHubSyncQueue::load(workspace.workspace_id, &queue_error);
  if (!needs_projection_restore && pending.isValid() &&
      workspace.sync_problem !=
          QLatin1String("working_copy_commit_required")) {
    staged_revision = pending.map_revision;
    setState(State::SavedLocally, tr("Saved locally — waiting to sync"));
  } else if (pending.isValid() &&
             workspace.sync_problem ==
                 QLatin1String("working_copy_commit_required")) {
    // The private snapshot is durable, but the provider-visible document is
    // still older. Keep the replay obligation live until the coordinated
    // working-copy commit succeeds.
    staged_revision = 0;
    stopped_for_conflict = true;
    setState(State::ActionRequired,
             tr("Saved recovery copy needs permission to update this map"));
    retry_timer->start(5000);
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

void MapHubSyncController::clear() { clearSession(true); }

void MapHubSyncController::clearSession(bool announce_away) {
  if (announce_away)
    sendFinalAwayHeartbeat();
  ++session_generation;
  for (auto *timer : {watch_timer, snapshot_idle_timer, snapshot_max_timer,
                      upload_idle_timer, retry_timer, poll_timer,
                      presence_expiry_timer})
    timer->stop();
  const auto clients =
      findChildren<MapHubApiClient *>(QString{}, Qt::FindDirectChildrenOnly);
  for (auto *client : clients)
    delete client;
  request_client = nullptr;
  QObject::disconnect(edit_connection);
  edit_connection = {};
  for (const auto &connection : entity_connections)
    QObject::disconnect(connection);
  entity_connections.clear();
  operation_store.reset();
  map = nullptr;
  workspace = {};
  revision_provider = {};
  snapshot_provider = {};
  working_copy_committer = {};
  observed_revision = 0;
  staged_revision = 0;
  semantic_revision = 0;
  upload_pending = false;
  poll_pending = false;
  pull_pending = false;
  restore_pending = false;
  stopped_for_conflict = false;
  acknowledgement_pending = false;
  acknowledgement_dirty = false;
  retry_outbox_now = false;
  structure_change_pending = false;
  applying_remote_operations = false;
  workspace_metadata_pending = false;
  workspace_metadata_desired_stopped = false;
  workspace_metadata_desired_problem.clear();
  workspace_metadata_error_message.clear();
  active_poll_interval_ms = poll_active_ms;
  idle_poll_interval_ms = 120'000;
  presence_ttl_seconds = 0;
  server_time = {};
  server_time_age.invalidate();
  presence.clear();
  compaction_map.reset();
  compaction_generation = 0;
  compaction_workspace_id.clear();
  compaction_target_sequence = 0;
  compaction_target_hash.clear();
  compaction_file_path.clear();
  local_entities.clear();
  setState(State::Disconnected, {});
  emit detailsChanged();
}

bool MapHubSyncController::editable() const {
  return workspace.isValid() && !isTerminalWorkspaceStatus(workspace.status);
}

int MapHubSyncController::pendingOperationCount() const {
  return operation_store ? operation_store->pendingCount() : 0;
}

QDateTime MapHubSyncController::estimatedServerTime() const {
  if (!server_time.isValid() || !server_time_age.isValid())
    return {};
  return server_time.addMSecs(server_time_age.elapsed());
}

void MapHubSyncController::schedulePresenceExpiry() {
  presence_expiry_timer->stop();
  const auto now = estimatedServerTime();
  if (!now.isValid() || presence_ttl_seconds <= 0 || presence.isEmpty())
    return;

  qint64 earliest_delay = std::numeric_limits<qint64>::max();
  for (const auto &collaborator : std::as_const(presence)) {
    if (!collaborator.last_seen_at.isValid())
      continue;
    // The UI treats age == TTL as fresh, so repaint immediately after that
    // boundary. Already-expired entries need no further timer.
    const auto expiry = collaborator.last_seen_at
                            .addSecs(presence_ttl_seconds)
                            .addMSecs(1);
    const auto delay = now.msecsTo(expiry);
    if (delay > 0)
      earliest_delay = std::min(earliest_delay, delay);
  }
  if (earliest_delay == std::numeric_limits<qint64>::max())
    return;

  // A malformed future last-seen value must not suppress repaint forever.
  // Normal entries fire exactly at expiry; future-skewed entries refresh at
  // least once per presence TTL until a poll replaces them.
  const auto bounded_delay = std::clamp<qint64>(
      earliest_delay, 1,
      std::max<qint64>(1000, qint64(presence_ttl_seconds) * 1000 + 1));
  presence_expiry_timer->start(
      int(std::min<qint64>(bounded_delay, std::numeric_limits<int>::max())));
}

bool MapHubSyncController::isCurrentSession(
    quint64 generation, const QString &workspace_id) const {
  return generation == session_generation && workspace.isValid() &&
         workspace.workspace_id == workspace_id;
}

bool MapHubSyncController::isCurrentCompactionSession(
    quint64 generation, const QString &workspace_id) const {
  return isCurrentSession(generation, workspace_id) && upload_pending &&
         request_client && compaction_generation == generation &&
         compaction_workspace_id == workspace_id;
}

void MapHubSyncController::retryNow() {
  if (!workspace.isValid())
    return;
  retry_timer->stop();
  if (workspace_metadata_pending ||
      workspace.sync_problem == QLatin1String("workspace_metadata")) {
    const auto generation = session_generation;
    const auto workspace_id = workspace.workspace_id;
    if (!saveWorkspace() || !isCurrentSession(generation, workspace_id))
      return;
    emit detailsChanged();
    if (workspace.sync_problem ==
        QLatin1String("working_copy_commit_required")) {
      retryWork();
      return;
    }
    if (workspace.sync_problem ==
        QLatin1String("snapshot_restore_required")) {
      restoreProjection();
      return;
    }
    QString inbox_error;
    const auto durable_inbox =
        operation_store
            ? operation_store->unappliedTransactions(&inbox_error)
            : QVector<MapHubCommittedTransaction>{};
    if (!inbox_error.isEmpty()) {
      setState(State::ActionRequired, inbox_error);
      return;
    }
    if (!durable_inbox.isEmpty()) {
      // A failed sidecar receipt deliberately leaves the inbox intact. Restage
      // the exact current editor revision before deleting it, so Retry remains
      // safe even if editing advanced after the metadata failure.
      if (!stageSnapshotNow() ||
          !isCurrentSession(generation, workspace_id))
        return;
    }
    if (stopped_for_conflict) {
      setState(State::ActionRequired,
               tr("Map Hub synchronization still needs attention"));
      return;
    }
    setState(workspace.checkpoint_required ? State::CheckpointNeeded
                                           : State::Watching,
             workspace.checkpoint_required
                 ? tr("Map settings saved on this device — checkpoint to share")
                 : tr("Map Hub metadata restored — checking for updates"));
    pollSyncState();
    if (workspace.initial_snapshot_required ||
        workspace.compaction_recommended || workspace.compaction_required)
      uploadPendingSnapshot();
    else
      drainOutbox();
    return;
  }
  if (workspace.sync_problem ==
      QLatin1String("working_copy_commit_required")) {
    retryWork();
    return;
  }
  if (workspace.sync_problem == QLatin1String("snapshot_restore_required")) {
    restoreProjection();
    return;
  }
  retry_outbox_now = true;
  pollSyncState();
  if (!stopped_for_conflict) {
    if (workspace.initial_snapshot_required ||
        workspace.compaction_recommended || workspace.compaction_required)
      uploadPendingSnapshot();
    else
      drainOutbox();
  }
}

bool MapHubSyncController::checkpointStreamHead(qint64 *sequence, QString *hash,
                                                QString *entity_index_sha256) {
  queueStructureChanges();
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
  application_active = true;
  poll_timer->setInterval(active_poll_interval_ms);
  watch_timer->start();
  poll_timer->start();
  acknowledgeAppliedOperations();
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
  application_active = false;
  watch_timer->stop();
  poll_timer->setInterval(idle_poll_interval_ms);
  observeRevision();
  if (observed_revision != staged_revision)
    stageSnapshot();
  acknowledgeAppliedOperations();
}

void MapHubSyncController::savedExplicitly() {
  if (!workspace.isValid())
    return;
  observeRevision();
  scheduleSnapshot(true);
}

bool MapHubSyncController::ensureWorkingCopyDurableNow() {
  if (!editable() || !revision_provider || !snapshot_provider)
    return false;
  const auto generation = session_generation;
  const auto workspace_id = workspace.workspace_id;

  // A structural signal is emitted before Map increments its revision. Flush
  // that deferred capture now so the durable receipt covers both semantic
  // operations and any checkpoint-only portion of this exact revision.
  structure_change_pending = false;
  if (!applying_remote_operations)
    queueStructureChanges();
  if (!isCurrentSession(generation, workspace_id))
    return false;
  observeRevision();
  if (!isCurrentSession(generation, workspace_id))
    return false;

  // Provider coordination can replay document events. Retry a bounded number
  // of times if that advances the map without replacing this session; success
  // means the provider-visible working copy and private recovery receipt both
  // name the exact current editor revision.
  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto target_revision = revision_provider();
    if (!target_revision)
      return false;
    if (observed_revision != target_revision) {
      observed_revision = target_revision;
      if (semantic_revision != target_revision)
        queueStructureChanges();
      if (!isCurrentSession(generation, workspace_id))
        return false;
    }
    if (!stageSnapshotNow(false) ||
        !isCurrentSession(generation, workspace_id))
      return false;
    const auto current_revision = revision_provider();
    if (current_revision && current_revision == target_revision &&
        staged_revision == target_revision)
      return true;
  }
  return false;
}

void MapHubSyncController::observeRevision() {
  if (!editable() || !revision_provider)
    return;
  const auto revision = revision_provider();
  if (!revision || revision == observed_revision)
    return;
  observed_revision = revision;
  if (semantic_revision != revision)
    queueStructureChanges();
  scheduleSnapshot();
}

void MapHubSyncController::scheduleSnapshot(bool urgent) {
  if (!editable())
    return;
  if (!snapshot_max_timer->isActive())
    snapshot_max_timer->start(snapshot_max_ms);
  snapshot_idle_timer->start(urgent ? 0 : snapshot_idle_ms);
}

void MapHubSyncController::stageSnapshot() { stageSnapshotNow(); }

bool MapHubSyncController::stageSnapshotNow(bool resume_after_inbox) {
  snapshot_idle_timer->stop();
  snapshot_max_timer->stop();
  if (!editable() || !snapshot_provider)
    return false;
  const auto state_before_stage = current_state;
  const auto generation = session_generation;
  const auto workspace_id = workspace.workspace_id;
  const auto provider = snapshot_provider;
  if (observed_revision == staged_revision)
    return finishDurableInbox(resume_after_inbox);
  auto directory = MapHubSyncQueue::workspaceDirectory(workspace_id);
  if (!QDir().mkpath(directory)) {
    setState(State::ActionRequired,
             tr("Cannot create the Map Hub recovery folder"));
    return false;
  }
  auto temporary_path = QDir(directory).filePath(
      QStringLiteral(".staged-%1.omap")
          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  setState(State::SavingLocally, tr("Saving locally…"));
  if (!isCurrentSession(generation, workspace_id))
    return false;
  quint64 snapshot_revision = 0;
  QString snapshot_error;
  if (!provider(temporary_path, &snapshot_revision, &snapshot_error)) {
    QFile::remove(temporary_path);
    if (!isCurrentSession(generation, workspace_id))
      return false;
    // Editing operations temporarily prevent serialization. Retry without
    // surfacing a modal failure.
    snapshot_idle_timer->start(5000);
    setState(State::Watching, snapshot_error.isEmpty()
                                  ? tr("Local save will retry")
                                  : snapshot_error);
    return false;
  }
  if (!isCurrentSession(generation, workspace_id)) {
    QFile::remove(temporary_path);
    return false;
  }
  QString hash_error;
  auto sha256 =
      MapHubApiClient::sha256ForFile(temporary_path, &hash_error).toLower();
  auto final_path = MapHubSyncQueue::snapshotPath(workspace_id, sha256);
  if (sha256.isEmpty() || final_path.isEmpty()) {
    QFile::remove(temporary_path);
    setState(State::ActionRequired,
             hash_error.isEmpty()
                 ? tr("Could not verify the local recovery copy")
                 : hash_error);
    return false;
  }
  if (!commitContentAddressedFile(temporary_path, final_path, sha256,
                                  QFileInfo(temporary_path).size())) {
    QFile::remove(temporary_path);
    setState(State::ActionRequired,
             tr("Could not commit the local Map Hub recovery copy"));
    return false;
  }
  MapHubPendingDraft pending;
  pending.workspace_id = workspace_id;
  pending.local_map_path = workspace.local_map_path;
  pending.snapshot_path = final_path;
  pending.sha256 = sha256;
  pending.size_bytes = QFileInfo(final_path).size();
  pending.map_revision = snapshot_revision;
  pending.expected_workspace_revision_id =
      workspace.active_revision_id.isEmpty() ? workspace.base_revision_id
                                             : workspace.active_revision_id;
  pending.expected_project_revision_id =
      workspace.stream_protocol == QLatin1String("oom-map-ops/1")
          ? workspace.project_revision_id
          : (workspace.project_revision_id.isEmpty()
                 ? workspace.base_revision_id
                 : workspace.project_revision_id);
  pending.idempotency_key = MapHubSyncQueue::idempotencyKey(
      pending.workspace_id, pending.expected_workspace_revision_id,
      pending.sha256);
  pending.staged_at = QDateTime::currentDateTimeUtc();
  QString queue_error;
  if (operation_store && operation_store->pendingCount() == 0 &&
      (workspace.stream_protocol.isEmpty() ||
       workspace.initial_snapshot_required ||
       workspace.compaction_recommended || workspace.compaction_required)) {
    const auto bootstrap = workspace.stream_protocol.isEmpty() ||
                           workspace.initial_snapshot_required;
    const auto index = bootstrap ? MapHubEntityIndex::bootstrap(*map)
                                 : operation_store->entityIndex(&queue_error);
    const auto bytes = index.canonicalBytes(&queue_error);
    pending.entity_index_sha256 = index.sha256(&queue_error);
    pending.entity_index_path = MapHubSyncQueue::entityIndexPath(
        workspace_id, pending.entity_index_sha256);
    pending.base_stream_sequence = index.stream_sequence;
    pending.base_stream_hash = index.stream_hash;
    pending.client_instance_id = workspace.client_instance_id;
    pending.bootstrap = bootstrap;
    pending.publish_snapshot =
        !bytes.isEmpty() && !pending.entity_index_path.isEmpty();
    if (!pending.publish_snapshot) {
      setState(State::ActionRequired, queue_error);
      return false;
    }
    QSaveFile index_file(pending.entity_index_path);
    if (!index_file.open(QIODevice::WriteOnly) ||
        index_file.write(bytes) != bytes.size() || !index_file.commit()) {
      setState(State::ActionRequired,
               index_file.errorString().isEmpty()
                   ? tr("Could not preserve the Map Hub entity index")
                   : index_file.errorString());
      return false;
    }
    QFile::setPermissions(pending.entity_index_path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    pending.idempotency_key = MapHubSyncQueue::idempotencyKey(
        pending.workspace_id, pending.expected_workspace_revision_id,
        pending.sha256, pending.base_stream_sequence, pending.base_stream_hash,
        pending.entity_index_sha256);
  }
  if (!MapHubSyncQueue::save(pending, &queue_error)) {
    setState(State::ActionRequired, queue_error);
    return false;
  }
  // A connected workspace is an autosaved working copy. On iOS the active
  // document must be committed through UIDocument; replacing its provider URL
  // directly is reported back to Mapper as an external edit. The private
  // operation log and content-addressed snapshot are durable, but their replay
  // obligation must remain live until the provider-visible document catches
  // up. Otherwise a later reopen could start from older bytes after the inbox
  // was already marked applied.
  if (!commitRequiredWorkingCopy(pending.snapshot_path, pending.size_bytes))
    return false;
  if (!isCurrentSession(generation, workspace_id))
    return false;
  if (!upload_pending)
    MapHubSyncQueue::pruneSnapshots(workspace_id,
                                    pending.snapshot_path, nullptr,
                                    pending.entity_index_path);
  staged_revision = snapshot_revision;
  // If editing advanced while serialization ran, immediately begin the next
  // coalescing window. The just-staged bytes remain a valid recovery point.
  observed_revision = revision_provider ? revision_provider() : staged_revision;
  const auto editing_advanced = observed_revision != staged_revision;
  const auto preserve_synced_state =
      state_before_stage == State::Synced && operation_store &&
      operation_store->pendingCount() == 0 &&
      !workspace.checkpoint_required && !workspace.initial_snapshot_required &&
      !workspace.compaction_recommended && !workspace.compaction_required &&
      !upload_pending && !pull_pending && workspace.sync_problem.isEmpty();
  setState(preserve_synced_state
               ? State::Synced
               : (workspace.checkpoint_required ? State::CheckpointNeeded
                                                : State::SavedLocally),
           preserve_synced_state
               ? tr("Synced to Map Hub")
               : (workspace.checkpoint_required
                      ? tr("Map settings saved locally — checkpoint to share")
                      : tr("Saved locally")));
  if (!isCurrentSession(generation, workspace_id))
    return false;
  if (pending.publish_snapshot)
    QTimer::singleShot(0, this, [this, generation, workspace_id] {
      if (isCurrentSession(generation, workspace_id))
        uploadPendingSnapshot();
    });
  if (editing_advanced)
    scheduleSnapshot();
  return finishDurableInbox(resume_after_inbox);
}

bool MapHubSyncController::finishDurableInbox(bool resume_after_inbox) {
  if (!workspace.isValid())
    return false;
  const auto generation = session_generation;
  const auto workspace_id = workspace.workspace_id;
  QString inbox_error;
  const auto durable_inbox =
      operation_store ? operation_store->unappliedTransactions(&inbox_error)
                      : QVector<MapHubCommittedTransaction>{};
  if (!inbox_error.isEmpty()) {
    setState(State::ActionRequired,
             inbox_error.isEmpty()
                 ? tr("Could not finish the durable Map Hub replay")
                 : inbox_error);
    return false;
  }
  if (!durable_inbox.isEmpty()) {
    const auto applied_sequence = durable_inbox.constLast().stream_sequence;
    workspace.applied_stream_sequence = applied_sequence;
    // Persist the receipt before deleting the replay journal. If the sidecar
    // write fails, a crash still leaves the inbox available for idempotent
    // recovery instead of losing the only durable applied-sequence evidence.
    if (!saveWorkspace())
      return false;
    if (!isCurrentSession(generation, workspace_id))
      return false;
    if (!operation_store->markTransactionsApplied(applied_sequence,
                                                  &inbox_error)) {
      setState(State::ActionRequired,
               inbox_error.isEmpty()
                   ? tr("Could not finish the durable Map Hub replay")
                   : inbox_error);
      return false;
    }
    acknowledgeAppliedOperations();
    if (resume_after_inbox)
      QTimer::singleShot(0, this, [this, generation, workspace_id] {
        if (isCurrentSession(generation, workspace_id))
          pullOperations();
      });
  }
  return isCurrentSession(generation, workspace_id);
}

QHash<QString, MapHubSyncController::LocalEntity>
MapHubSyncController::captureLocalEntities() const {
  QHash<QString, LocalEntity> result;
  if (!map)
    return result;

  QString previous;
  for (int p = 0; p < map->getNumParts(); ++p) {
    const auto *part = map->getPart(p);
    result.insert(part->persistentId(),
                  {QStringLiteral("part"), {}, previous, part->getName()});
    previous = part->persistentId();

    QString previous_object;
    for (int o = 0; o < part->getNumObjects(); ++o) {
      const auto *object = part->getObject(o);
      result.insert(object->persistentId(), {QStringLiteral("object"),
                                             part->persistentId(),
                                             previous_object,
                                             {}});
      previous_object = object->persistentId();
    }
  }

  previous.clear();
  for (int s = 0; s < map->getNumSymbols(); ++s) {
    const auto *symbol = map->getSymbol(s);
    result.insert(symbol->persistentId(),
                  {QStringLiteral("symbol"),
                   {},
                   previous,
                   MapHubEditTransaction::symbolFragment(*symbol, *map)});
    previous = symbol->persistentId();
  }
  return result;
}

bool MapHubSyncController::enqueueOperations(
    QVector<MapHubEditOperation> operations, QString *error) {
  if (operations.isEmpty())
    return true;
  if (!operation_store) {
    if (error)
      *error = QStringLiteral("The Map Hub operation store is unavailable.");
    return false;
  }
  const auto state = operation_store->state(error);
  const auto pending_count = operation_store->pendingCount(error);
  const auto versions = operation_store->entityVersions(error);
  if (state.client_instance_id.isEmpty() || pending_count < 0 ||
      (error && !error->isEmpty()))
    return false;
  for (auto &operation : operations)
    operation.expected_version = versions.value(operation.entity_id, 0);

  MapHubEditTransaction transaction;
  transaction.client_instance_id = state.client_instance_id;
  transaction.client_sequence =
      state.acknowledged_client_sequence + pending_count + 1;
  transaction.transaction_id =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  transaction.expected_stream_sequence = state.optimistic_stream_sequence;
  transaction.expected_stream_hash = state.optimistic_stream_hash;
  transaction.expected_workspace_revision_id =
      workspace.active_revision_id.isEmpty() ? workspace.base_revision_id
                                             : workspace.active_revision_id;
  transaction.expected_project_revision_id =
      workspace.stream_protocol == QLatin1String("oom-map-ops/1")
          ? workspace.project_revision_id
          : (workspace.project_revision_id.isEmpty()
                 ? workspace.base_revision_id
                 : workspace.project_revision_id);
  transaction.operations = std::move(operations);
  return transaction.isValid(error) &&
         operation_store->enqueue(transaction, error);
}

void MapHubSyncController::refreshObjectCache(
    const QVector<MapHubEditOperation> &operations) {
  if (!map)
    return;
  QSet<QString> affected_parts;
  for (const auto &operation : operations) {
    if (operation.entityKind() != QLatin1String("object"))
      continue;
    const auto old = local_entities.constFind(operation.entity_id);
    if (old != local_entities.cend() && !old->parent_id.isEmpty())
      affected_parts.insert(old->parent_id);
    if (!operation.parent_id.isEmpty())
      affected_parts.insert(operation.parent_id);
    if (operation.isDelete())
      local_entities.remove(operation.entity_id);
  }
  for (const auto &part_id : affected_parts) {
    auto *part = locatePart(*map, part_id);
    if (!part)
      continue;
    QString previous;
    for (int o = 0; o < part->getNumObjects(); ++o) {
      const auto *object = part->getObject(o);
      local_entities.insert(object->persistentId(),
                            {QStringLiteral("object"), part_id, previous, {}});
      previous = object->persistentId();
    }
  }
}

void MapHubSyncController::queueStructureChanges() {
  if (!map || !operation_store || applying_remote_operations || !editable() ||
      workspace.sync_problem == QLatin1String("local_operation_queue"))
    return;
  const auto colors_dirty = map->areColorsDirty();
  const auto templates_dirty = map->areTemplatesDirty();
  const auto other_dirty = map->isOtherDirty();
  const auto current_revision = revision_provider ? revision_provider() : 0;
  const auto current = captureLocalEntities();
  QVector<MapHubEditOperation> operations;

  auto append_put = [&operations](const QString &id,
                                  const LocalEntity &entity) {
    if (entity.kind == QLatin1String("part")) {
      operations.push_back({MapHubEditOperation::Kind::PutPart,
                            id,
                            {},
                            entity.after_id,
                            0,
                            entity.payload});
    } else if (entity.kind == QLatin1String("symbol")) {
      operations.push_back({MapHubEditOperation::Kind::PutSymbol,
                            id,
                            {},
                            entity.after_id,
                            0,
                            entity.payload});
    }
  };

  // Creates and replacements are emitted in final list order, so every
  // ordering anchor is already live when Map Hub validates the transaction.
  for (int p = 0; p < map->getNumParts(); ++p) {
    const auto id = map->getPart(p)->persistentId();
    if (!local_entities.contains(id) || local_entities.value(id) != current[id])
      append_put(id, current[id]);
  }
  for (int s = 0; s < map->getNumSymbols(); ++s) {
    const auto id = map->getSymbol(s)->persistentId();
    if (!local_entities.contains(id) || local_entities.value(id) != current[id])
      append_put(id, current[id]);
  }
  for (int p = 0; p < map->getNumParts(); ++p) {
    const auto *part = map->getPart(p);
    for (int o = 0; o < part->getNumObjects(); ++o) {
      const auto *object = part->getObject(o);
      const auto id = object->persistentId();
      const auto old = local_entities.constFind(id);
      if (old == local_entities.cend() ||
          old->kind != QLatin1String("object") ||
          old->parent_id != current[id].parent_id ||
          old->after_id != current[id].after_id) {
        operations.push_back({MapHubEditOperation::Kind::PutObject, id,
                              current[id].parent_id, current[id].after_id, 0,
                              MapHubEditTransaction::objectFragment(*object)});
      }
    }
  }

  QStringList deleted_objects;
  QStringList deleted_symbols;
  QStringList deleted_parts;
  for (auto it = local_entities.cbegin(); it != local_entities.cend(); ++it) {
    if (current.contains(it.key()))
      continue;
    if (it->kind == QLatin1String("object"))
      deleted_objects.push_back(it.key());
    else if (it->kind == QLatin1String("symbol"))
      deleted_symbols.push_back(it.key());
    else if (it->kind == QLatin1String("part"))
      deleted_parts.push_back(it.key());
  }
  for (auto *ids : {&deleted_objects, &deleted_symbols, &deleted_parts})
    ids->sort();
  for (const auto &id : deleted_objects)
    operations.push_back(
        {MapHubEditOperation::Kind::DeleteObject, id, {}, {}, 0, {}});
  for (const auto &id : deleted_symbols)
    operations.push_back(
        {MapHubEditOperation::Kind::DeleteSymbol, id, {}, {}, 0, {}});
  for (const auto &id : deleted_parts)
    operations.push_back(
        {MapHubEditOperation::Kind::DeletePart, id, {}, {}, 0, {}});

  if (operations.isEmpty()) {
    if (current_revision && semantic_revision != current_revision)
      markCheckpointRequired();
    local_entities = current;
    semantic_revision = current_revision;
    return;
  }

  // Map parts use the broad `other_dirty` bit even though their complete
  // create/delete/reorder representation is in this batch. Suppress that one
  // noisy checkpoint only when the deferred signal owns exactly the next map
  // revision. A prior unsupported edit, compound change, explicit color or
  // template change, or existing checkpoint obligation remains latched.
  const auto contains_part_operation =
      std::any_of(operations.cbegin(), operations.cend(), [](const auto &op) {
        return op.kind == MapHubEditOperation::Kind::PutPart ||
               op.kind == MapHubEditOperation::Kind::DeletePart;
      });
  const auto other_dirty_is_pure_part =
      other_dirty && contains_part_operation && !colors_dirty &&
      !templates_dirty && !workspace.checkpoint_required && current_revision &&
      semantic_revision < std::numeric_limits<quint64>::max() &&
      current_revision == semantic_revision + 1;
  const auto needs_complete_checkpoint =
      colors_dirty || templates_dirty ||
      (other_dirty && !other_dirty_is_pure_part);

  // Structural changes are not represented by Mapper's object UndoSteps.
  // Bound each durable batch comfortably below the protocol's decoded limits.
  qsizetype offset = 0;
  while (offset < operations.size()) {
    qsizetype count = 0;
    qsizetype payload_bytes = 0;
    while (offset + count < operations.size() && count < 240) {
      const auto bytes = operations[offset + count].payload.toUtf8().size();
      if (count > 0 && payload_bytes + bytes > 350 * 1024)
        break;
      payload_bytes += bytes;
      ++count;
    }
    auto batch = operations.mid(offset, count);
    QString error;
    if (!enqueueOperations(batch, &error)) {
      stopped_for_conflict = true;
      workspace.sync_problem = QStringLiteral("local_operation_queue");
      saveWorkspace();
      setState(State::ActionRequired,
               error.isEmpty()
                   ? tr("Could not preserve this map structure change for "
                        "Map Hub")
                   : error);
      return;
    }
    for (const auto &operation : batch) {
      if (operation.isDelete())
        local_entities.remove(operation.entity_id);
      else
        local_entities.insert(operation.entity_id,
                              current.value(operation.entity_id));
    }
    offset += count;
  }
  local_entities = current;
  semantic_revision = current_revision;
  staged_revision = 0;
  if (needs_complete_checkpoint)
    markCheckpointRequired();
  emit detailsChanged();
  setState(State::SavedLocally, tr("Saved locally"));
  upload_idle_timer->start(250);
}

void MapHubSyncController::queueCommittedEdit(const UndoStep *step) {
  if (!step || !map || !operation_store || applying_remote_operations ||
      !editable() ||
      workspace.sync_problem == QLatin1String("local_operation_queue"))
    return;
  std::vector<UndoStep::EntityChange> changes;
  step->collectEntityChanges(changes);
  if (changes.empty()) {
    const auto revision = revision_provider ? revision_provider() : 0;
    if (revision && semantic_revision != revision)
      markCheckpointRequired();
    semantic_revision = revision;
    return;
  }
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
      workspace.stream_protocol == QLatin1String("oom-map-ops/1")
          ? workspace.project_revision_id
          : (workspace.project_revision_id.isEmpty()
                 ? workspace.base_revision_id
                 : workspace.project_revision_id),
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
  refreshObjectCache(transaction.operations);
  semantic_revision = revision_provider ? revision_provider() : 0;
  staged_revision = 0;
  emit detailsChanged();
  setState(State::SavedLocally, tr("Saved locally"));
  upload_idle_timer->start(250);
}

void MapHubSyncController::scheduleStructureChanges() {
  if (structure_change_pending || !workspace.isValid())
    return;
  structure_change_pending = true;
  const auto generation = session_generation;
  const auto workspace_id = workspace.workspace_id;
  QTimer::singleShot(0, this, [this, generation, workspace_id] {
    structure_change_pending = false;
    if (isCurrentSession(generation, workspace_id) &&
        !applying_remote_operations)
      queueStructureChanges();
  });
}

void MapHubSyncController::drainOutbox() {
  if (!editable() || stopped_for_conflict || upload_pending || !operation_store)
    return;
  QString error;
  const auto pending = operation_store->nextPending(&error);
  if (!pending.isValid()) {
    retry_outbox_now = false;
    if (!error.isEmpty())
      setState(State::ActionRequired, error);
    else
      setState(State::Synced, tr("Synced to Map Hub"));
    return;
  }
  const auto bypass_backoff = std::exchange(retry_outbox_now, false);
  if (workspace.stream_protocol != QLatin1String("oom-map-ops/1")) {
    setState(State::SavedLocally, tr("Saved locally — preparing Map Hub sync"));
    return;
  }
  const auto now = QDateTime::currentDateTimeUtc();
  if (!bypass_backoff && pending.next_attempt_at.isValid() &&
      pending.next_attempt_at > now) {
    retry_timer->start(
        std::max<qint64>(1, now.msecsTo(pending.next_attempt_at)));
    return;
  }
  auto account = MapHubCredentials::readToken(workspace.server_url);
  auto lease = MapHubCredentials::readWorkspaceLease(
      workspace.server_url, workspace.workspace_id,
      workspace.client_instance_id);
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
      workspace.workspace_id, pending.canonical_json,
      workspace.client_instance_id, lease.token,
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
              request_error.code == QLatin1String("invalid_anchor") ||
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
              request_error.code == QLatin1String("unsupported_protocol") ||
              request_error.code == QLatin1String("unsupported_operation") ||
              request_error.code == QLatin1String("client_sequence_gap") ||
              request_error.code == QLatin1String("idempotency_conflict") ||
              request_error.code == QLatin1String("snapshot_required") ||
              request_error.code == QLatin1String("invalid_payload") ||
              request_error.code == QLatin1String("payload_too_large") ||
              request_error.code == QLatin1String("unauthorized") ||
              request_error.code == QLatin1String("workspace_unavailable") ||
              (request_error.http_status >= 400 &&
               request_error.http_status < 500 &&
               request_error.http_status != 408 &&
               request_error.http_status != 429)) {
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
          const auto retry_at = request_error.retry_after.isValid()
                                    ? request_error.retry_after
                                    : QDateTime::currentDateTimeUtc().addSecs(
                                          base_seconds + jitter);
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
        const auto response_versions =
            response.value(QStringLiteral("entity_versions")).toArray();
        auto valid_versions =
            response_versions.size() == pending.transaction.operations.size();
        for (qsizetype i = 0; valid_versions && i < response_versions.size();
             ++i) {
          const auto version = response_versions[i].toObject();
          const auto &operation = pending.transaction.operations[i];
          valid_versions =
              !version.isEmpty() &&
              version.value(QStringLiteral("entity_kind")).toString() ==
                  operation.entityKind() &&
              version.value(QStringLiteral("entity_id")).toString() ==
                  operation.entity_id &&
              version.value(QStringLiteral("version")).toInteger(-1) ==
                  operation.expected_version + 1;
        }
        QString store_error;
        if (response.value(QStringLiteral("protocol")).toString() !=
                QLatin1String("oom-map-ops/1") ||
            response.value(QStringLiteral("transaction_id")).toString() !=
                pending.transaction_id ||
            response.value(QStringLiteral("payload_sha256")).toString() !=
                pending.payload_sha256 ||
            sequence != pending.predicted_stream_sequence ||
            hash != pending.predicted_stream_hash ||
            acknowledged != pending.client_sequence || !valid_versions ||
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
        emit detailsChanged();
        observed_revision = revision_provider ? revision_provider() : 0;
        if (!stageSnapshotNow(false))
          return;
        QTimer::singleShot(0, this, &MapHubSyncController::drainOutbox);
      });
}

void MapHubSyncController::preparePublishedCompaction() {
  if (upload_pending || stopped_for_conflict || !operation_store || !editable())
    return;
  QString error;
  const auto state = operation_store->state(&error);
  const auto pending_count = operation_store->pendingCount(&error);
  if (!error.isEmpty() || pending_count <= 0 ||
      state.published_stream_sequence < workspace.snapshot_stream_sequence ||
      state.published_stream_hash.size() != 64 ||
      workspace.snapshot_stream_sequence < 0 ||
      workspace.snapshot_stream_hash.size() != 64 ||
      workspace.snapshot_sha256.size() != 64 ||
      workspace.snapshot_download_url.isEmpty() ||
      workspace.snapshot_entity_index_sha256.size() != 64 ||
      workspace.snapshot_entity_index_download_url.isEmpty()) {
    failPublishedCompaction(
        error.isEmpty()
            ? tr("Map Hub cannot reconstruct the published compaction point")
            : error,
        false);
    return;
  }
  const auto account = MapHubCredentials::readToken(workspace.server_url);
  const auto lease = MapHubCredentials::readWorkspaceLease(
      workspace.server_url, workspace.workspace_id,
      workspace.client_instance_id);
  if (!account || account.token.isEmpty() || !lease || lease.token.isEmpty()) {
    failPublishedCompaction(
        tr("Reopen this Map Hub map to renew its connection"), false);
    return;
  }
  const auto directory =
      MapHubSyncQueue::workspaceDirectory(workspace.workspace_id);
  if (!QDir().mkpath(directory)) {
    failPublishedCompaction(
        tr("Cannot create the Map Hub compaction workspace"), false);
    return;
  }

  compaction_target_sequence = state.published_stream_sequence;
  compaction_target_hash = state.published_stream_hash;
  compaction_file_path = MapHubSyncQueue::snapshotPath(
      workspace.workspace_id, workspace.snapshot_sha256);
  if (compaction_file_path.isEmpty()) {
    failPublishedCompaction(
        tr("Map Hub returned an invalid compaction snapshot"), false);
    return;
  }
  request_client =
      new MapHubApiClient(workspace.server_url, account.token, this);
  upload_pending = true;
  const auto generation = session_generation;
  const auto workspace_id = workspace.workspace_id;
  compaction_generation = generation;
  compaction_workspace_id = workspace_id;
  setState(State::Syncing, tr("Reconstructing published Map Hub history…"));
  if (!isCurrentCompactionSession(generation, workspace_id))
    return;

  const auto load_snapshot =
      [this, generation, workspace_id](
          const QString &path,
          const MapHubApiClient::Error &download_error) {
    if (!isCurrentCompactionSession(generation, workspace_id))
      return;
    if (download_error) {
      failPublishedCompaction(download_error.message,
                              retryableRequestError(download_error));
      return;
    }
    compaction_map = std::make_unique<Map>();
    if (!compaction_map->loadFrom(path)) {
      failPublishedCompaction(
          tr("The verified Map Hub snapshot could not be opened"), false);
      return;
    }
    request_client->workspaceEntityIndex(
        QUrl(workspace.snapshot_entity_index_download_url),
        [this, generation, workspace_id](
            const QJsonObject &response,
            const MapHubApiClient::Error &index_error) {
          if (!isCurrentCompactionSession(generation, workspace_id))
            return;
          if (index_error) {
            failPublishedCompaction(index_error.message,
                                    retryableRequestError(index_error));
            return;
          }
          QString validation_error;
          const auto index =
              MapHubEntityIndex::fromJson(response, &validation_error);
          const auto canonical = index.canonicalBytes(&validation_error);
          const auto digest = QString::fromLatin1(
              QCryptographicHash::hash(canonical, QCryptographicHash::Sha256)
                  .toHex());
          if (canonical.isEmpty() ||
              digest != workspace.snapshot_entity_index_sha256 ||
              index.stream_sequence != workspace.snapshot_stream_sequence ||
              index.stream_hash != workspace.snapshot_stream_hash ||
              !index.matchesMapTopology(*compaction_map, &validation_error)) {
            failPublishedCompaction(
                validation_error.isEmpty()
                    ? tr("The Map Hub compaction snapshot did not verify")
                    : validation_error,
                false);
            return;
          }
          if (index.stream_sequence == compaction_target_sequence) {
            if (index.stream_hash != compaction_target_hash) {
              failPublishedCompaction(
                  tr("The Map Hub compaction head did not verify"), false);
              return;
            }
            finishPublishedCompaction(generation, workspace_id);
          } else {
            pullPublishedCompactionTail(index.stream_sequence,
                                        index.stream_hash, generation,
                                        workspace_id);
          }
        });
  };

  QString hash_error;
  if (QFileInfo::exists(compaction_file_path) &&
      MapHubApiClient::sha256ForFile(compaction_file_path, &hash_error) ==
          workspace.snapshot_sha256) {
    QTimer::singleShot(
        0, this,
        [this, load_snapshot, path = compaction_file_path, generation,
         workspace_id] {
          if (isCurrentCompactionSession(generation, workspace_id))
            load_snapshot(path, {});
        });
  } else {
    QFile::remove(compaction_file_path);
    request_client->downloadArtifact(QUrl(workspace.snapshot_download_url),
                                     workspace.snapshot_sha256,
                                     compaction_file_path, load_snapshot);
  }
}

void MapHubSyncController::pullPublishedCompactionTail(
    qint64 after_sequence, const QString &after_hash, quint64 generation,
    const QString &workspace_id) {
  if (!isCurrentCompactionSession(generation, workspace_id))
    return;
  if (!compaction_map || after_sequence < workspace.snapshot_stream_sequence ||
      after_sequence >= compaction_target_sequence ||
      !validStreamHash(after_hash)) {
    failPublishedCompaction(
        tr("The Map Hub compaction replay state is invalid"), false);
    return;
  }
  const auto limit =
      int(std::min<qint64>(256, compaction_target_sequence - after_sequence));
  request_client->workspaceOperations(
      workspace_id, after_sequence, limit,
      [this, generation, workspace_id, after_sequence,
       after_hash](const QJsonObject &response,
                   const MapHubApiClient::Error &request_error) {
        if (!isCurrentCompactionSession(generation, workspace_id))
          return;
        if (request_error) {
          if (request_error.code == QLatin1String("snapshot_required")) {
            workspace.sync_etag.clear();
            saveWorkspace();
            failPublishedCompaction(
                tr("Map Hub advanced its retained snapshot; refreshing…"),
                true);
            QTimer::singleShot(0, this,
                               [this, generation, workspace_id] {
              if (isCurrentSession(generation, workspace_id))
                pollSyncState();
            });
          } else {
            failPublishedCompaction(request_error.message,
                                    retryableRequestError(request_error));
          }
          return;
        }
        QVector<MapHubCommittedTransaction> transactions;
        QString error;
        qint64 sequence = after_sequence;
        auto hash = after_hash;
        for (const auto &value :
             response.value(QStringLiteral("transactions")).toArray()) {
          if (!value.isObject()) {
            error = QStringLiteral("Map Hub returned an invalid transaction.");
            break;
          }
          auto committed =
              MapHubCommittedTransaction::fromJson(value.toObject(), &error);
          if (!committed.isValid(&error) ||
              committed.stream_sequence != sequence + 1 ||
              committed.transaction.expected_stream_sequence != sequence ||
              committed.transaction.expected_stream_hash != hash ||
              MapHubOperationStore::chainHash(hash, committed.payload_sha256) !=
                  committed.stream_hash ||
              committed.stream_sequence > compaction_target_sequence) {
            if (error.isEmpty())
              error = QStringLiteral(
                  "Map Hub returned a discontinuous compaction tail.");
            break;
          }
          sequence = committed.stream_sequence;
          hash = committed.stream_hash;
          transactions.push_back(std::move(committed));
        }
        const auto response_after =
            response.value(QStringLiteral("after")).toInteger(-1);
        const auto next_after =
            response.value(QStringLiteral("next_after")).toInteger(-1);
        const auto response_head =
            response.value(QStringLiteral("head_sequence")).toInteger(-1);
        if (response.value(QStringLiteral("protocol")).toString() !=
                QLatin1String("oom-map-ops/1") ||
            response_after != after_sequence || next_after != sequence ||
            response_head < compaction_target_sequence ||
            !validStreamHash(
                response.value(QStringLiteral("head_hash")).toString()) ||
            !response.value(QStringLiteral("has_more")).isBool() ||
            (transactions.isEmpty() && sequence < compaction_target_sequence)) {
          if (error.isEmpty())
            error = QStringLiteral(
                "Map Hub returned an inconsistent compaction page.");
        }
        QVector<MapHubEditTransaction> operation_transactions;
        for (const auto &committed : transactions)
          operation_transactions.push_back(committed.transaction);
        if (error.isEmpty() &&
            !applyMapTransactions(*compaction_map, operation_transactions,
                                  false, &error)) {
          // applyMapTransactions provides the actionable validation error.
        }
        if (!error.isEmpty()) {
          failPublishedCompaction(error, false);
          return;
        }
        if (sequence == compaction_target_sequence) {
          if (hash != compaction_target_hash) {
            failPublishedCompaction(
                tr("The reconstructed Map Hub stream hash did not verify"),
                false);
            return;
          }
          finishPublishedCompaction(generation, workspace_id);
        } else {
          pullPublishedCompactionTail(sequence, hash, generation,
                                      workspace_id);
        }
      });
}

void MapHubSyncController::finishPublishedCompaction(
    quint64 generation, const QString &workspace_id) {
  if (!isCurrentCompactionSession(generation, workspace_id) ||
      !compaction_map || !operation_store)
    return;
  QString error;
  const auto state = operation_store->state(&error);
  const auto index = operation_store->entityIndex(&error);
  const auto index_bytes = index.canonicalBytes(&error);
  const auto index_sha = index.sha256(&error);
  if (!error.isEmpty() ||
      state.published_stream_sequence != compaction_target_sequence ||
      state.published_stream_hash != compaction_target_hash ||
      index.stream_sequence != compaction_target_sequence ||
      index.stream_hash != compaction_target_hash || index_bytes.isEmpty() ||
      index_sha.isEmpty() ||
      !index.matchesMapTopology(*compaction_map, &error)) {
    failPublishedCompaction(
        error.isEmpty()
            ? tr("The reconstructed Map Hub projection did not verify")
            : error,
        false);
    return;
  }

  const auto directory =
      MapHubSyncQueue::workspaceDirectory(workspace.workspace_id);
  const auto temporary_path = QDir(directory).filePath(
      QStringLiteral(".published-%1.omap")
          .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  XMLFileExporter exporter(temporary_path, compaction_map.get(), nullptr);
  if (!exporter.doExport()) {
    QFile::remove(temporary_path);
    failPublishedCompaction(
        tr("Mapper could not serialize the published Map Hub state"), false);
    return;
  }
  const auto sha256 =
      MapHubApiClient::sha256ForFile(temporary_path, &error).toLower();
  const auto final_path =
      MapHubSyncQueue::snapshotPath(workspace.workspace_id, sha256);
  const auto size_bytes = QFileInfo(temporary_path).size();
  if (sha256.isEmpty() || final_path.isEmpty() || size_bytes < 1 ||
      !commitContentAddressedFile(temporary_path, final_path, sha256,
                                  size_bytes)) {
    QFile::remove(temporary_path);
    failPublishedCompaction(
        error.isEmpty()
            ? tr("Mapper could not preserve the published compaction copy")
            : error,
        false);
    return;
  }
  compaction_file_path = final_path;

  const auto account = MapHubCredentials::readToken(workspace.server_url);
  const auto lease = MapHubCredentials::readWorkspaceLease(
      workspace.server_url, workspace.workspace_id,
      workspace.client_instance_id);
  if (!account || account.token.isEmpty() || !lease || lease.token.isEmpty()) {
    failPublishedCompaction(
        tr("Reopen this Map Hub map to renew its connection"), false);
    return;
  }
  const auto expected_revision = workspace.active_revision_id.isEmpty()
                                     ? workspace.base_revision_id
                                     : workspace.active_revision_id;
  const auto idempotency_key = MapHubSyncQueue::idempotencyKey(
      workspace_id, expected_revision, sha256,
      compaction_target_sequence, compaction_target_hash, index_sha);
  setState(State::Syncing, tr("Compacting published Map Hub history…"));
  if (!isCurrentCompactionSession(generation, workspace_id))
    return;
  request_client->uploadWorkspaceSnapshot(
      workspace_id, final_path, index_bytes,
      compaction_target_sequence, compaction_target_hash, sha256, size_bytes,
      expected_revision, workspace.project_revision_id,
      workspace.client_instance_id, lease.token, idempotency_key,
      [this, generation, workspace_id, sha256, size_bytes,
       index_sha](const QJsonObject &response,
                  const MapHubApiClient::Error &request_error) {
        if (!isCurrentCompactionSession(generation, workspace_id))
          return;
        if (request_error) {
          failPublishedCompaction(request_error.message,
                                  retryableRequestError(request_error));
          return;
        }
        const auto snapshot =
            response.value(QStringLiteral("snapshot")).toObject();
        const auto response_index =
            snapshot.value(QStringLiteral("entity_index")).toObject();
        if (response.value(QStringLiteral("protocol")).toString() !=
                QLatin1String("oom-map-ops/1") ||
            snapshot.value(QStringLiteral("base_stream_sequence"))
                    .toInteger(-1) != compaction_target_sequence ||
            snapshot.value(QStringLiteral("base_stream_hash")).toString() !=
                compaction_target_hash ||
            snapshot.value(QStringLiteral("sha256")).toString() != sha256 ||
            response_index.value(QStringLiteral("sha256")).toString() !=
                index_sha) {
          failPublishedCompaction(
              tr("Map Hub returned an invalid compaction acknowledgement"),
              false);
          return;
        }

        workspace.compaction_recommended = false;
        workspace.compaction_required = false;
        workspace.uncompacted_operations = 0;
        workspace.snapshot_stream_sequence = compaction_target_sequence;
        workspace.snapshot_stream_hash = compaction_target_hash;
        workspace.snapshot_id = snapshot.value(QStringLiteral("id")).toString();
        workspace.snapshot_sha256 = sha256;
        workspace.snapshot_size_bytes = size_bytes;
        workspace.snapshot_download_url =
            snapshot.value(QStringLiteral("download_url")).toString();
        workspace.snapshot_revision_id =
            snapshot.value(QStringLiteral("revision_id")).toString();
        workspace.snapshot_entity_index_sha256 = index_sha;
        workspace.snapshot_entity_index_download_url =
            response_index.value(QStringLiteral("download_url")).toString();
        workspace.last_synced_at = QDateTime::currentDateTimeUtc();
        workspace.sync_problem.clear();
        saveWorkspace();

        const auto completed_path = compaction_file_path;
        QString queue_error;
        const auto recovery =
            MapHubSyncQueue::load(workspace_id, &queue_error);
        if (!recovery.isValid() || recovery.snapshot_path != completed_path)
          QFile::remove(completed_path);
        upload_pending = false;
        if (request_client) {
          request_client->deleteLater();
          request_client = nullptr;
        }
        compaction_map.reset();
        compaction_generation = 0;
        compaction_workspace_id.clear();
        compaction_target_sequence = 0;
        compaction_target_hash.clear();
        compaction_file_path.clear();
        setState(State::SavedLocally,
                 tr("Published history compacted — resuming sync"));
        QTimer::singleShot(0, this, [this, generation, workspace_id] {
          if (!isCurrentSession(generation, workspace_id))
            return;
          pollSyncState();
          drainOutbox();
        });
      });
}

void MapHubSyncController::failPublishedCompaction(const QString &message,
                                                   bool retryable) {
  upload_pending = false;
  if (request_client) {
    request_client->deleteLater();
    request_client = nullptr;
  }
  compaction_map.reset();
  compaction_generation = 0;
  compaction_workspace_id.clear();
  compaction_target_sequence = 0;
  compaction_target_hash.clear();
  compaction_file_path.clear();
  if (retryable) {
    // Start before emitting stateChanged. A reentrant document switch calls
    // clear(), which must be able to cancel this old session's retry.
    retry_timer->start(5000);
    setState(State::WaitingForNetwork,
             message.isEmpty()
                 ? tr("Published history is safe — compaction will retry")
                 : message);
  } else {
    stopped_for_conflict = true;
    workspace.sync_problem = QStringLiteral("published_compaction_failed");
    saveWorkspace();
    setState(State::ActionRequired,
             message.isEmpty()
                 ? tr("Map Hub published-history compaction needs attention")
                 : message);
  }
}

void MapHubSyncController::uploadPendingSnapshot() {
  if ((!workspace.initial_snapshot_required &&
       !workspace.compaction_recommended && !workspace.compaction_required) ||
      upload_pending || stopped_for_conflict || !editable())
    return;
  if (workspace.compaction_required && operation_store &&
      operation_store->pendingCount() > 0) {
    preparePublishedCompaction();
    return;
  }
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
  const auto lease = MapHubCredentials::readWorkspaceLease(
      workspace.server_url, workspace.workspace_id,
      workspace.client_instance_id);
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
      pending.expected_project_revision_id, workspace.client_instance_id,
      lease.token, pending.idempotency_key,
      [this, pending](const QJsonObject &response,
                      const MapHubApiClient::Error &error) {
        upload_pending = false;
        if (request_client) {
          request_client->deleteLater();
          request_client = nullptr;
        }
        if (error) {
          if (!retryableRequestError(error) ||
              error.code == QLatin1String("stale_workspace_revision") ||
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
        if (workspace.stream_head_sequence <= pending.base_stream_sequence) {
          workspace.stream_head_sequence = pending.base_stream_sequence;
          workspace.stream_head_hash = pending.base_stream_hash;
        }
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
          if (!commitRequiredWorkingCopy(pending.snapshot_path,
                                         pending.size_bytes))
            return;
          MapHubSyncQueue::remove(workspace.workspace_id);
        }
        setState(State::Synced, tr("Connected editing ready"));
        QTimer::singleShot(0, this, &MapHubSyncController::pollSyncState);
      });
}

void MapHubSyncController::retryWork() {
  if (workspace.sync_problem ==
      QLatin1String("working_copy_commit_required")) {
    retryRequiredWorkingCopy();
  } else if (workspace.sync_problem ==
             QLatin1String("snapshot_restore_required")) {
    restoreProjection();
  } else if (workspace.initial_snapshot_required ||
             workspace.compaction_recommended || workspace.compaction_required)
    uploadPendingSnapshot();
  else
    drainOutbox();
}

void MapHubSyncController::acknowledgeAppliedOperations() {
  if (acknowledgement_pending) {
    acknowledgement_dirty = true;
    return;
  }
  if (!operation_store || workspace.applied_stream_sequence < 0)
    return;
  if (workspace.sync_problem ==
      QLatin1String("working_copy_commit_required")) {
    acknowledgement_dirty = true;
    return;
  }
  const auto lease = MapHubCredentials::readWorkspaceLease(
      workspace.server_url, workspace.workspace_id,
      workspace.client_instance_id);
  const auto account = MapHubCredentials::readToken(workspace.server_url);
  const auto state = operation_store->state();
  if (!lease || lease.token.isEmpty() || !account || account.token.isEmpty() ||
      state.client_instance_id.isEmpty()) {
    acknowledgement_dirty = true;
    return;
  }
  auto *client = new MapHubApiClient(workspace.server_url, account.token, this);
  const auto generation = session_generation;
  const auto workspace_id = workspace.workspace_id;
  const auto applied_sequence = workspace.applied_stream_sequence;
  const auto presence_state = application_active ? QStringLiteral("editing")
                                                 : QStringLiteral("away");
  acknowledgement_pending = true;
  acknowledgement_dirty = false;
  client->acknowledgeWorkspaceOperations(
      workspace_id,
      {{QStringLiteral("protocol"), QStringLiteral("oom-map-ops/1")},
       {QStringLiteral("client_instance_id"), state.client_instance_id},
       {QStringLiteral("applied_stream_sequence"),
        applied_sequence},
       {QStringLiteral("presence"),
        QJsonObject{{QStringLiteral("state"), presence_state},
                    {QStringLiteral("device_name"), presenceDeviceName()}}}},
      workspace.client_instance_id, lease.token,
      [this, client, generation, workspace_id, applied_sequence,
       presence_state](const QJsonObject &,
                       const MapHubApiClient::Error &error) {
        if (!isCurrentSession(generation, workspace_id)) {
          client->deleteLater();
          return;
        }
        acknowledgement_pending = false;
        client->deleteLater();
        if (error) {
          acknowledgement_dirty = true;
          if (!retryableRequestError(error)) {
            stopped_for_conflict = true;
            workspace.sync_problem =
                error.code.isEmpty()
                    ? QStringLiteral("acknowledgement_failed")
                    : error.code;
            saveWorkspace();
            setState(State::ActionRequired,
                     error.message.isEmpty()
                         ? tr("Map Hub could not confirm this editing session")
                         : error.message);
          }
          return;
        }

        const auto desired_presence =
            application_active ? QStringLiteral("editing")
                               : QStringLiteral("away");
        const auto needs_follow_up =
            acknowledgement_dirty ||
            workspace.applied_stream_sequence != applied_sequence ||
            desired_presence != presence_state;
        if (needs_follow_up)
          acknowledgeAppliedOperations();
      });
}

void MapHubSyncController::sendFinalAwayHeartbeat() {
  if (!operation_store || !workspace.isValid() ||
      workspace.applied_stream_sequence < 0 ||
      !QCoreApplication::instance())
    return;
  const auto state = operation_store->state();
  const auto lease = MapHubCredentials::readWorkspaceLease(
      workspace.server_url, workspace.workspace_id,
      workspace.client_instance_id);
  const auto account = MapHubCredentials::readToken(workspace.server_url);
  if (!lease || lease.token.isEmpty() || !account || account.token.isEmpty() ||
      state.client_instance_id.isEmpty())
    return;

  // Controller-owned requests are cancelled below. Give this advisory final
  // heartbeat application lifetime so a routine close leaves presence quickly
  // without revoking the lease needed for seamless reopen.
  auto *client = new MapHubApiClient(workspace.server_url, account.token,
                                     QCoreApplication::instance());
  client->acknowledgeWorkspaceOperations(
      workspace.workspace_id,
      {{QStringLiteral("protocol"), QStringLiteral("oom-map-ops/1")},
       {QStringLiteral("client_instance_id"), state.client_instance_id},
       {QStringLiteral("applied_stream_sequence"),
        workspace.applied_stream_sequence},
       {QStringLiteral("presence"),
        QJsonObject{{QStringLiteral("state"), QStringLiteral("away")},
                    {QStringLiteral("device_name"), presenceDeviceName()}}}},
      workspace.client_instance_id, lease.token,
      [client](const QJsonObject &, const MapHubApiClient::Error &) {
        client->deleteLater();
      });
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
  const auto generation = session_generation;
  const auto workspace_id = workspace.workspace_id;
  workspace.sync_problem = QStringLiteral("snapshot_restore_required");
  saveWorkspace();
  restore_pending = true;
  client->workspaceEntityIndex(
      QUrl(workspace.snapshot_entity_index_download_url),
      [this, client, generation, workspace_id](
          const QJsonObject &response,
          const MapHubApiClient::Error &request_error) {
        if (!isCurrentSession(generation, workspace_id)) {
          client->deleteLater();
          return;
        }
        restore_pending = false;
        client->deleteLater();
        if (request_error) {
          if (!retryableRequestError(request_error)) {
            stopped_for_conflict = true;
            workspace.sync_problem = request_error.code;
            saveWorkspace();
            setState(State::ActionRequired, request_error.message);
            return;
          }
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
        const auto prior_state = operation_store->state(&restore_error);
        const auto pending_count =
            operation_store->pendingCount(&restore_error);
        if (restore_error.isEmpty() && pending_count > 0 &&
            prior_state.published_stream_sequence > index.stream_sequence) {
          restore_error = QStringLiteral(
              "The downloaded recovery snapshot predates locally queued "
              "work. The original local workspace is still preserved.");
        }
        if (bytes.isEmpty() ||
            digest != workspace.snapshot_entity_index_sha256 ||
            index.stream_sequence != workspace.snapshot_stream_sequence ||
            index.stream_hash != workspace.snapshot_stream_hash ||
            !restore_error.isEmpty() ||
            !operation_store->rebasePendingOntoSnapshot(
                index,
                workspace.active_revision_id.isEmpty()
                    ? workspace.base_revision_id
                    : workspace.active_revision_id,
                workspace.project_revision_id, &restore_error)) {
          workspace.sync_problem =
              QStringLiteral("invalid_snapshot_entity_index");
          saveWorkspace();
          setState(State::ActionRequired,
                   restore_error.isEmpty()
                       ? tr("The Map Hub recovery index did not verify")
                       : restore_error);
          return;
        }
        const auto pending =
            operation_store->pendingTransactions(&restore_error);
        applying_remote_operations = true;
        const auto pending_applied =
            restore_error.isEmpty() &&
            applyMapTransactions(*map, pending, true, &restore_error);
        applying_remote_operations = false;
        if (!pending_applied) {
          stopped_for_conflict = true;
          workspace.sync_problem =
              QStringLiteral("snapshot_pending_replay_failed");
          saveWorkspace();
          setState(State::ActionRequired,
                   restore_error.isEmpty()
                       ? tr("Locally saved edits could not be replayed")
                       : restore_error);
          return;
        }
        stopped_for_conflict = false;
        workspace.sync_problem.clear();
        workspace.applied_stream_sequence = index.stream_sequence;
        if (!pending.isEmpty()) {
          map->setObjectsDirty();
          map->requestRedraw();
          observed_revision = revision_provider ? revision_provider() : 0;
          staged_revision = 0;
          if (!stageSnapshotNow(false))
            return;
        }
        local_entities = captureLocalEntities();
        semantic_revision = revision_provider ? revision_provider() : 0;
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
  const auto generation = session_generation;
  const auto workspace_id = workspace.workspace_id;
  const auto requested_after = local_state.published_stream_sequence;
  const auto requested_hash = local_state.published_stream_hash;
  pull_pending = true;
  setState(State::Syncing, tr("Applying upstream changes…"));
  client->workspaceOperations(
      workspace_id, requested_after, 256,
      [this, client, generation, workspace_id, requested_after,
       requested_hash](
          const QJsonObject &response, const MapHubApiClient::Error &error) {
        if (!isCurrentSession(generation, workspace_id)) {
          client->deleteLater();
          return;
        }
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
          } else if (retryableRequestError(error)) {
            setState(State::WaitingForNetwork,
                     tr("Saved locally — waiting for connection"));
          } else {
            stopped_for_conflict = true;
            workspace.sync_problem = error.code;
            saveWorkspace();
            setState(State::ActionRequired, error.message);
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
        const auto response_after =
            response.value(QStringLiteral("after")).toInteger(-1);
        const auto next_after =
            response.value(QStringLiteral("next_after")).toInteger(-1);
        const auto response_head =
            response.value(QStringLiteral("head_sequence")).toInteger(-1);
        const auto response_head_hash =
            response.value(QStringLiteral("head_hash")).toString();
        const auto has_more =
            response.value(QStringLiteral("has_more")).toBool();
        const auto expected_next =
            transactions.isEmpty() ? requested_after
                                   : transactions.constLast().stream_sequence;
        const auto expected_next_hash =
            transactions.isEmpty() ? requested_hash
                                   : transactions.constLast().stream_hash;
        if (response_after != requested_after || next_after != expected_next ||
            response_head < next_after ||
            !validStreamHash(response_head_hash) ||
            !response.value(QStringLiteral("has_more")).isBool() ||
            has_more != (next_after < response_head) ||
            (has_more && transactions.isEmpty()) ||
            (!has_more && response_head_hash != expected_next_hash)) {
          transaction_error = QStringLiteral(
              "Map Hub returned an inconsistent operation page.");
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
        const auto last_received = transactions.constLast();
        while (!transactions.isEmpty()) {
          const auto pending = operation_store->nextPending(&transaction_error);
          const auto &committed = transactions.constFirst();
          if (!pending.isValid() ||
              pending.transaction_id != committed.transaction.transaction_id ||
              pending.payload_sha256 != committed.payload_sha256 ||
              pending.predicted_stream_sequence != committed.stream_sequence ||
              pending.predicted_stream_hash != committed.stream_hash)
            break;
          if (!operation_store->acknowledge(
                  pending.client_sequence, committed.stream_sequence,
                  committed.stream_hash, &transaction_error))
            break;
          workspace.acknowledged_client_sequence = pending.client_sequence;
          transactions.removeFirst();
        }
        if (!transaction_error.isEmpty()) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("invalid_acknowledgement");
          saveWorkspace();
          setState(State::ActionRequired, transaction_error);
          return;
        }
        if (!transactions.isEmpty() &&
            !operation_store->rebaseOnto(transactions, &transaction_error)) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("entity_conflict");
          saveWorkspace();
          setState(State::UpstreamChanged,
                   tr("Upstream and local edits need review"));
          emit upstreamChangeDetected(transaction_error);
          return;
        }

        const auto unapplied =
            operation_store->unappliedTransactions(&transaction_error);
        QVector<MapHubEditTransaction> unapplied_transactions;
        for (const auto &committed : unapplied)
          unapplied_transactions.push_back(committed.transaction);
        applying_remote_operations = true;
        const auto applied = applyMapTransactions(*map, unapplied_transactions,
                                                  true, &transaction_error);
        applying_remote_operations = false;
        if (!applied) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("local_projection_mismatch");
          saveWorkspace();
          setState(State::ActionRequired, transaction_error);
          return;
        }
        map->setObjectsDirty();
        map->requestRedraw();
        local_entities = captureLocalEntities();
        semantic_revision = revision_provider ? revision_provider() : 0;
        observed_revision = semantic_revision;
        if (!unapplied.isEmpty())
          staged_revision = 0;
        workspace.stream_head_sequence = last_received.stream_sequence;
        workspace.stream_head_hash = last_received.stream_hash;
        workspace.last_synced_at = QDateTime::currentDateTimeUtc();
        workspace.sync_problem.clear();
        saveWorkspace();
        emit detailsChanged();
        if (!stageSnapshotNow(false))
          return;
        if (has_more)
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
  const auto lease = MapHubCredentials::readWorkspaceLease(
      workspace.server_url, workspace.workspace_id,
      workspace.client_instance_id);
  const auto editing_lease = lease ? lease.token : QString{};
  const auto generation = session_generation;
  const auto workspace_id = workspace.workspace_id;
  const auto client_instance_id = operation_store
                                      ? operation_store->state().client_instance_id
                                      : workspace.client_instance_id;
  poll_pending = true;
  client->workspaceSyncState(
      workspace_id, workspace.sync_etag, editing_lease, client_instance_id,
      [this, client, generation, workspace_id](
          const QJsonObject &response, const QString &etag, bool not_modified,
          const MapHubApiClient::Error &error) mutable {
        if (!isCurrentSession(generation, workspace_id)) {
          client->deleteLater();
          return;
        }
        poll_pending = false;
        client->deleteLater();
        // Presence freshness advances even when sync state does not. Repaint
        // on every completed poll so repeated errors and 304 responses cannot
        // leave a stale collaborator list on screen.
        emit detailsChanged();
        if (error) {
          if (!retryableRequestError(error)) {
            stopped_for_conflict = true;
            workspace.sync_problem = error.code;
            saveWorkspace();
            setState(State::ActionRequired, error.message);
          } else {
            setState(State::WaitingForNetwork,
                     tr("Saved locally — waiting for connection"));
          }
          return;
        }
        if (not_modified) {
          acknowledgeAppliedOperations();
          if (!stopped_for_conflict) {
            if (operation_store && operation_store->pendingCount() > 0)
              QTimer::singleShot(0, this,
                                 &MapHubSyncController::drainOutbox);
            else
              setState(State::Synced, tr("Synced to Map Hub"));
          }
          return;
        }
        if (!etag.isEmpty())
          workspace.sync_etag = etag;

        const auto response_server_time = QDateTime::fromString(
            response.value(QStringLiteral("server_time")).toString(),
            Qt::ISODate);
        if (response_server_time.isValid()) {
          server_time = response_server_time;
          server_time_age.start();
        }
        const auto sync = response.value(QStringLiteral("sync")).toObject();
        const auto suggested_active =
            sync.value(QStringLiteral("poll_after_ms")).toInteger(-1);
        const auto suggested_idle =
            sync.value(QStringLiteral("idle_poll_after_ms")).toInteger(-1);
        const auto suggested_presence_ttl =
            sync.value(QStringLiteral("presence_ttl_seconds")).toInteger(-1);
        if (suggested_active > 0)
          active_poll_interval_ms = std::clamp<int>(
              int(std::min<qint64>(suggested_active,
                                   std::numeric_limits<int>::max())),
              poll_active_min_ms, poll_active_max_ms);
        if (suggested_idle > 0)
          idle_poll_interval_ms = std::clamp<int>(
              int(std::min<qint64>(suggested_idle,
                                   std::numeric_limits<int>::max())),
              poll_idle_min_ms, poll_idle_max_ms);
        if (suggested_presence_ttl > 0)
          presence_ttl_seconds = std::clamp<int>(
              int(std::min<qint64>(suggested_presence_ttl, 3600)), 1, 3600);
        poll_timer->setInterval(application_active ? active_poll_interval_ms
                                                   : idle_poll_interval_ms);

        QVector<Collaborator> updated_presence;
        const auto presence_values =
            response.value(QStringLiteral("presence")).toArray();
        updated_presence.reserve(presence_values.size());
        for (const auto &value : presence_values) {
          if (!value.isObject())
            continue;
          const auto object = value.toObject();
          Collaborator collaborator;
          collaborator.person_id =
              object.value(QStringLiteral("person_id")).toString();
          collaborator.client_instance_id =
              object.value(QStringLiteral("client_instance_id")).toString();
          collaborator.display_name =
              object.value(QStringLiteral("display_name")).toString();
          collaborator.last_seen_at = QDateTime::fromString(
              object.value(QStringLiteral("last_seen_at")).toString(),
              Qt::ISODate);
          collaborator.state =
              object.value(QStringLiteral("state")).toString();
          collaborator.applied_stream_sequence =
              object.value(QStringLiteral("applied_stream_sequence"))
                  .toInteger(-1);
          collaborator.is_current_user =
              object.value(QStringLiteral("is_current_user")).toBool();
          if (collaborator.person_id.isEmpty() ||
              collaborator.client_instance_id.isEmpty() ||
              collaborator.display_name.isEmpty() ||
              !collaborator.last_seen_at.isValid() ||
              collaborator.applied_stream_sequence < 0)
            continue;
          updated_presence.push_back(std::move(collaborator));
        }
        presence = std::move(updated_presence);
        schedulePresenceExpiry();
        emit detailsChanged();
        const auto workspace_object =
            response.value(QStringLiteral("workspace")).toObject();
        const auto workspace_revision =
            response.value(QStringLiteral("workspace_revision")).toObject();
        const auto project_revision =
            response.value(QStringLiteral("project_revision")).toObject();
        const auto stream = response.value(QStringLiteral("stream")).toObject();
        if (response.value(QStringLiteral("canonical_json")).toString() !=
                QLatin1String("oom-json/1") ||
            response.value(QStringLiteral("protocol")).toString() !=
                QLatin1String("oom-map-ops/1") ||
            stream.isEmpty()) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("unsupported_protocol");
          saveWorkspace();
          setState(State::ActionRequired,
                   tr("Map Hub returned unsupported synchronization state"));
          return;
        }
        const auto protocol =
            response.value(QStringLiteral("protocol")).toString();
        const auto head_sequence =
            stream.value(QStringLiteral("head_sequence")).toInteger(-1);
        const auto head_hash =
            stream.value(QStringLiteral("head_hash")).toString();
        const auto minimum_sequence =
            stream.value(QStringLiteral("minimum_available_sequence"))
                .toInteger(-1);
        const auto initial_snapshot_required =
            stream.value(QStringLiteral("initial_snapshot_required")).toBool();
        const auto uncompacted_operations =
            stream.value(QStringLiteral("uncompacted_operations"))
                .toInteger(-1);
        const auto valid_minimum =
            minimum_sequence >= 1 &&
            (minimum_sequence <= head_sequence ||
             (head_sequence < std::numeric_limits<qint64>::max() &&
              minimum_sequence == head_sequence + 1));
        if (protocol != QLatin1String("oom-map-ops/1") || head_sequence < 0 ||
            !validStreamHash(head_hash) || !valid_minimum ||
            uncompacted_operations < 0 ||
            !stream.value(QStringLiteral("initial_snapshot_required"))
                 .isBool() ||
            !stream.value(QStringLiteral("compaction_recommended")).isBool() ||
            !stream.value(QStringLiteral("compaction_required")).isBool()) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("invalid_sync_state");
          saveWorkspace();
          setState(State::ActionRequired,
                   tr("Map Hub returned invalid synchronization state"));
          return;
        }
        workspace.stream_protocol = protocol;
        workspace.stream_head_sequence = head_sequence;
        workspace.stream_head_hash = head_hash;
        workspace.minimum_available_sequence = minimum_sequence;
        workspace.initial_snapshot_required = initial_snapshot_required;
        workspace.uncompacted_operations = uncompacted_operations;
        workspace.compaction_recommended =
            stream.value(QStringLiteral("compaction_recommended")).toBool();
        workspace.compaction_required =
            stream.value(QStringLiteral("compaction_required")).toBool();
        const auto snapshot =
            stream.value(QStringLiteral("snapshot")).toObject();
        if (!snapshot.isEmpty()) {
          const auto snapshot_sequence =
              snapshot.value(QStringLiteral("base_stream_sequence"))
                  .toInteger(-1);
          const auto snapshot_hash =
              snapshot.value(QStringLiteral("base_stream_hash")).toString();
          const auto snapshot_id =
              snapshot.value(QStringLiteral("id")).toString();
          const auto snapshot_sha =
              snapshot.value(QStringLiteral("sha256")).toString();
          const auto snapshot_size =
              snapshot.value(QStringLiteral("size_bytes")).toInteger(-1);
          const auto snapshot_download =
              snapshot.value(QStringLiteral("download_url")).toString();
          const auto entity_index =
              snapshot.value(QStringLiteral("entity_index")).toObject();
          const auto index_sha =
              entity_index.value(QStringLiteral("sha256")).toString();
          const auto index_download =
              entity_index.value(QStringLiteral("download_url")).toString();
          if (QUuid(snapshot_id).isNull() || snapshot_sequence < 0 ||
              snapshot_sequence > head_sequence ||
              !validStreamHash(snapshot_hash) ||
              !validStreamHash(snapshot_sha) || snapshot_size < 1 ||
              QUrl(snapshot_download).isEmpty() ||
              !validStreamHash(index_sha) || QUrl(index_download).isEmpty()) {
            stopped_for_conflict = true;
            workspace.sync_problem = QStringLiteral("invalid_sync_snapshot");
            saveWorkspace();
            setState(State::ActionRequired,
                     tr("Map Hub returned an invalid recovery snapshot"));
            return;
          }
          workspace.snapshot_stream_sequence = snapshot_sequence;
          workspace.snapshot_stream_hash = snapshot_hash;
          workspace.snapshot_id = snapshot_id;
          workspace.snapshot_sha256 = snapshot_sha;
          workspace.snapshot_size_bytes = snapshot_size;
          workspace.snapshot_download_url = snapshot_download;
          workspace.snapshot_revision_id =
              snapshot.value(QStringLiteral("revision_id")).toString();
          workspace.snapshot_entity_index_sha256 = index_sha;
          workspace.snapshot_entity_index_download_url = index_download;
        } else {
          workspace.snapshot_stream_sequence = 0;
          workspace.snapshot_stream_hash.clear();
          workspace.snapshot_id.clear();
          workspace.snapshot_sha256.clear();
          workspace.snapshot_size_bytes = 0;
          workspace.snapshot_download_url.clear();
          workspace.snapshot_revision_id.clear();
          workspace.snapshot_entity_index_sha256.clear();
          workspace.snapshot_entity_index_download_url.clear();
          if (!initial_snapshot_required || head_sequence != 0) {
            stopped_for_conflict = true;
            workspace.sync_problem = QStringLiteral("missing_sync_snapshot");
            saveWorkspace();
            setState(State::ActionRequired,
                     tr("Map Hub did not provide the required recovery "
                        "snapshot"));
            return;
          }
        }
        const auto lease = response.value(QStringLiteral("lease")).toObject();
        const auto lease_expiry = QDateTime::fromString(
            lease.value(QStringLiteral("expires_at")).toString(), Qt::ISODate);
        if (lease_expiry.isValid())
          workspace.lease_expires_at = lease_expiry;
        if (!lease.value(QStringLiteral("valid")).toBool()) {
          stopped_for_conflict = true;
          workspace.sync_problem = QStringLiteral("lease_required");
          setState(State::ActionRequired,
                   tr("Reopen this Map Hub map to renew its editing lease"));
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
        const auto accepting_terminal_status =
            isTerminalWorkspaceStatus(remote_status) &&
            !isTerminalWorkspaceStatus(workspace.status);
        if (accepting_terminal_status) {
          // The status below makes stageSnapshotNow() intentionally refuse
          // further writes. First capture the exact current editor revision,
          // including checkpoint-only map settings, in both private recovery
          // storage and the provider-visible working copy.
          if (!ensureWorkingCopyDurableNow()) {
            if (!isCurrentSession(generation, workspace_id))
              return;
            // This response cannot be acknowledged by ETag yet. Force a full
            // refetch after the local save retry so a 304 cannot strand the
            // workspace as editable forever.
            workspace.sync_etag.clear();
            saveWorkspace();
            emit detailsChanged();
            QTimer::singleShot(5000, this,
                               [this, generation, workspace_id] {
              if (isCurrentSession(generation, workspace_id))
                pollSyncState();
            });
            return;
          }
          if (!isCurrentSession(generation, workspace_id))
            return;
        }
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
        emit detailsChanged();
        acknowledgeAppliedOperations();
        if ((workspace.compaction_recommended ||
             workspace.compaction_required) &&
            operation_store) {
          if (operation_store->pendingCount() == 0) {
            staged_revision = 0;
            scheduleSnapshot(true);
          } else if (workspace.compaction_required) {
            QTimer::singleShot(
                0, this, &MapHubSyncController::preparePublishedCompaction);
          } else {
            QTimer::singleShot(0, this, &MapHubSyncController::drainOutbox);
          }
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
  if (workspace_metadata_pending &&
      !workspace_metadata_error_message.isEmpty() &&
      state != State::Disconnected &&
      (state != State::ActionRequired ||
       text != workspace_metadata_error_message))
    return;
  if (stopped_for_conflict && state != State::Disconnected &&
      state != State::UpstreamChanged && state != State::ActionRequired)
    return;
  if (workspace.checkpoint_required &&
      (state == State::Watching || state == State::SavedLocally ||
       state == State::Synced)) {
    state = State::CheckpointNeeded;
    setState(state,
             tr("Map settings saved on this device — checkpoint to share"));
    return;
  }
  if (current_state == state && state_text == text)
    return;
  current_state = state;
  state_text = text;
  emit stateChanged(state, text);
}

void MapHubSyncController::markCheckpointRequired() {
  if (!workspace.isValid())
    return;
  if (!workspace.checkpoint_required) {
    workspace.checkpoint_required = true;
    saveWorkspace();
    emit detailsChanged();
  }
  setState(State::CheckpointNeeded,
           tr("Map settings saved on this device — checkpoint to share"));
  staged_revision = 0;
  scheduleSnapshot(true);
}

bool MapHubSyncController::saveWorkspace() {
  auto durable_workspace = workspace;
  if (workspace_metadata_pending) {
    if (workspace.sync_problem != QLatin1String("workspace_metadata")) {
      workspace_metadata_desired_problem = workspace.sync_problem;
      workspace_metadata_desired_stopped =
          !workspace.sync_problem.isEmpty() &&
          workspace.sync_problem != QLatin1String("compaction_required");
    }
    durable_workspace.sync_problem = workspace_metadata_desired_problem;
  }
  QString error;
  if (ManagedMapWorkspace::save(durable_workspace, &error)) {
    if (workspace_metadata_pending) {
      workspace = durable_workspace;
      stopped_for_conflict = workspace_metadata_desired_stopped;
      workspace_metadata_pending = false;
      workspace_metadata_desired_stopped = false;
      workspace_metadata_desired_problem.clear();
      workspace_metadata_error_message.clear();
    }
    return true;
  }
  if (!workspace_metadata_pending) {
    workspace_metadata_desired_problem = workspace.sync_problem;
    workspace_metadata_desired_stopped = stopped_for_conflict;
  }
  workspace_metadata_pending = true;
  workspace.sync_problem = QStringLiteral("workspace_metadata");
  stopped_for_conflict = true;
  workspace_metadata_error_message =
      tr("Could not update Map Hub sync metadata: %1").arg(error);
  setState(State::ActionRequired, workspace_metadata_error_message);
  return false;
}

bool MapHubSyncController::commitWorkingCopy(const QString &snapshot_path,
                                             qint64 expected_size,
                                             QString *error) {
  if (error)
    error->clear();
  const auto committer = working_copy_committer;
  if (committer)
    return committer(snapshot_path, expected_size, error);
  const auto committed = copyFileAtomically(
      snapshot_path, workspace.local_map_path, expected_size);
  if (!committed && error)
    *error = tr("Mapper could not update the working .omap file");
  return committed;
}

bool MapHubSyncController::commitRequiredWorkingCopy(
    const QString &snapshot_path, qint64 expected_size) {
  if (!workspace.isValid())
    return false;
  const auto generation = session_generation;
  const auto workspace_id = workspace.workspace_id;
  QString working_copy_error;
  const auto committed =
      commitWorkingCopy(snapshot_path, expected_size, &working_copy_error);
  if (!isCurrentSession(generation, workspace_id))
    return false;
  if (!committed) {
    stopped_for_conflict = true;
    workspace.sync_problem =
        QStringLiteral("working_copy_commit_required");
    saveWorkspace();
    if (!isCurrentSession(generation, workspace_id))
      return false;
    setState(
        State::ActionRequired,
        working_copy_error.isEmpty()
            ? tr("The recovery copy is safe, but Mapper could not update this map")
            : tr("The recovery copy is safe, but this map could not be updated: %1")
                  .arg(working_copy_error));
    if (!isCurrentSession(generation, workspace_id))
      return false;
    retry_timer->start(5000);
    return false;
  }

  if (workspace.sync_problem ==
      QLatin1String("working_copy_commit_required")) {
    workspace.sync_problem.clear();
    stopped_for_conflict = false;
    if (!saveWorkspace()) {
      if (!isCurrentSession(generation, workspace_id))
        return false;
      return false;
    }
  }
  return isCurrentSession(generation, workspace_id);
}

bool MapHubSyncController::retryRequiredWorkingCopy() {
  QString queue_error;
  const auto pending =
      MapHubSyncQueue::load(workspace.workspace_id, &queue_error);
  QString hash_error;
  const QFileInfo snapshot(pending.snapshot_path);
  const auto verified =
      pending.isValid() && snapshot.isFile() &&
      snapshot.size() == pending.size_bytes &&
      MapHubApiClient::sha256ForFile(pending.snapshot_path, &hash_error)
              .compare(pending.sha256, Qt::CaseInsensitive) == 0;
  if (!verified) {
    retry_timer->stop();
    stopped_for_conflict = true;
    workspace.sync_problem = QStringLiteral("working_copy_recovery_invalid");
    saveWorkspace();
    setState(State::ActionRequired,
             !queue_error.isEmpty()
                 ? queue_error
                 : (!hash_error.isEmpty()
                        ? hash_error
                        : tr("The saved Map Hub recovery copy did not verify")));
    return false;
  }
  if (!commitRequiredWorkingCopy(pending.snapshot_path, pending.size_bytes))
    return false;

  staged_revision = pending.map_revision;
  if (!upload_pending)
    MapHubSyncQueue::pruneSnapshots(workspace.workspace_id,
                                    pending.snapshot_path, nullptr,
                                    pending.entity_index_path);
  const auto already_published =
      pending.publish_snapshot && !workspace.snapshot_id.isEmpty() &&
      workspace.snapshot_sha256.compare(pending.sha256,
                                        Qt::CaseInsensitive) == 0;
  if (already_published)
    MapHubSyncQueue::remove(workspace.workspace_id);

  setState(workspace.checkpoint_required ? State::CheckpointNeeded
                                         : State::SavedLocally,
           workspace.checkpoint_required
               ? tr("Map settings saved locally — checkpoint to share")
               : tr("Saved locally"));
  if (!finishDurableInbox(true))
    return false;

  observed_revision = revision_provider ? revision_provider() : staged_revision;
  if (observed_revision != staged_revision)
    scheduleSnapshot();
  if (pending.publish_snapshot && !already_published)
    QTimer::singleShot(0, this,
                       &MapHubSyncController::uploadPendingSnapshot);
  else if (operation_store && operation_store->pendingCount() > 0)
    QTimer::singleShot(0, this, &MapHubSyncController::drainOutbox);
  else
    QTimer::singleShot(0, this, &MapHubSyncController::pollSyncState);
  return true;
}

} // namespace OpenOrienteering
