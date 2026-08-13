/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_sync_controller.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>

#include "collaboration/map_hub_api_client.h"
#include "collaboration/map_hub_credentials.h"
#include "collaboration/map_hub_sync_queue.h"

namespace OpenOrienteering {

namespace {

constexpr auto watch_interval_ms = 500;
constexpr auto stage_idle_ms = 1500;
constexpr auto retry_interval_ms = 5000;
constexpr auto poll_interval_ms = 15000;

bool retryable(const MapHubApiClient::Error &error) {
  return error.http_status == 0 || error.http_status == 408 ||
         error.http_status == 429 || error.http_status >= 500;
}

QString uploadKey(const QString &workspace_id, const QString &base_version_id,
                  const QString &sha256) {
  const auto material = workspace_id + QLatin1Char('|') + base_version_id +
                        QLatin1Char('|') + sha256;
  return QStringLiteral("file-%1").arg(QString::fromLatin1(
      QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha256)
          .toHex()
          .left(48)));
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

} // namespace

MapHubSyncController::MapHubSyncController(QObject *parent)
    : QObject(parent), watch_timer(new QTimer(this)),
      stage_timer(new QTimer(this)), retry_timer(new QTimer(this)),
      poll_timer(new QTimer(this)) {
  watch_timer->setInterval(watch_interval_ms);
  stage_timer->setSingleShot(true);
  retry_timer->setSingleShot(true);
  poll_timer->setInterval(poll_interval_ms);
  connect(watch_timer, &QTimer::timeout, this,
          &MapHubSyncController::observeRevision);
  connect(stage_timer, &QTimer::timeout, this,
          &MapHubSyncController::stageAndUpload);
  connect(retry_timer, &QTimer::timeout, this,
          &MapHubSyncController::stageAndUpload);
  connect(poll_timer, &QTimer::timeout, this,
          &MapHubSyncController::pollFileState);
}

MapHubSyncController::~MapHubSyncController() = default;

void MapHubSyncController::configure(const ManagedMapWorkspace &new_workspace,
                                     Map *new_map,
                                     RevisionProvider new_revision_provider,
                                     SnapshotProvider new_snapshot_provider,
                                     WorkingCopyCommitter
                                         new_working_copy_committer) {
  clear();
  if (!new_workspace.isValid() || !new_map || !new_revision_provider ||
      !new_snapshot_provider)
    return;
  workspace = new_workspace;
  map = new_map;
  revision_provider = std::move(new_revision_provider);
  snapshot_provider = std::move(new_snapshot_provider);
  working_copy_committer = std::move(new_working_copy_committer);
  observed_revision = revision_provider();
  staged_revision = observed_revision;
  workspace.file_protocol = QStringLiteral("omap-snapshot/1");
  if (QUuid(workspace.client_instance_id).isNull())
    workspace.client_instance_id =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
  saveWorkspace();
  setState(State::Watching, tr("Saved on this device"));
  watch_timer->start();
  poll_timer->start();
  pollFileState();
  if (workspace.file_version_id.isEmpty())
    stage_timer->start(0);
}

void MapHubSyncController::clear() {
  for (auto *timer : {watch_timer, stage_timer, retry_timer, poll_timer})
    timer->stop();
  if (request_client) {
    request_client->deleteLater();
    request_client = nullptr;
  }
  workspace = {};
  map = nullptr;
  revision_provider = {};
  snapshot_provider = {};
  working_copy_committer = {};
  observed_revision = 0;
  staged_revision = 0;
  upload_pending = false;
  poll_pending = false;
  stopped_for_conflict = false;
  staged_path.clear();
  staged_sha256.clear();
  setState(State::Disconnected, {});
}

void MapHubSyncController::applicationBecameActive() {
  if (!workspace.isValid())
    return;
  watch_timer->start();
  poll_timer->start();
  pollFileState();
  if (!staged_path.isEmpty())
    stage_timer->start(0);
}

void MapHubSyncController::applicationWillResignActive() {
  if (!workspace.isValid())
    return;
  observeRevision();
  if (!upload_pending && !stopped_for_conflict)
    stageAndUpload();
}

void MapHubSyncController::savedExplicitly() {
  if (!workspace.isValid())
    return;
  observed_revision = revision_provider ? revision_provider() : 0;
  stage_timer->start(0);
}

void MapHubSyncController::observeRevision() {
  if (!editable() || !revision_provider)
    return;
  const auto revision = revision_provider();
  if (!revision || revision == observed_revision)
    return;
  observed_revision = revision;
  setState(State::SavedLocally, tr("Saved on this device"));
  stage_timer->start(stage_idle_ms);
}

void MapHubSyncController::stageAndUpload() {
  stage_timer->stop();
  if (!editable() || upload_pending || stopped_for_conflict ||
      !snapshot_provider)
    return;
  const auto directory =
      MapHubSyncQueue::workspaceDirectory(workspace.workspace_id);
  if (directory.isEmpty() || !QDir().mkpath(directory)) {
    setState(State::ActionRequired,
             tr("Your map is safe, but Mapper could not create its local "
                "handoff folder."));
    return;
  }
  const auto temporary_path =
      QDir(directory).filePath(QStringLiteral("handoff-staging.omap"));
  quint64 snapshot_revision = 0;
  QString error;
  setState(State::SavingLocally, tr("Saving on this device…"));
  if (!snapshot_provider(temporary_path, &snapshot_revision, &error)) {
    setState(State::SavedLocally,
             error.isEmpty() ? tr("Saved on this device — upload will retry")
                             : error);
    retry_timer->start(retry_interval_ms);
    return;
  }
  const auto sha256 =
      MapHubApiClient::sha256ForFile(temporary_path, &error).toLower();
  const auto size_bytes = QFileInfo(temporary_path).size();
  const auto final_path =
      MapHubSyncQueue::snapshotPath(workspace.workspace_id, sha256);
  if (sha256.size() != 64 || size_bytes <= 0 || final_path.isEmpty()) {
    setState(State::ActionRequired,
             error.isEmpty() ? tr("Your map is safe, but its handoff copy "
                                  "could not be verified.")
                             : error);
    return;
  }
  if (temporary_path != final_path) {
    if (!QFileInfo::exists(final_path) &&
        !QFile::copy(temporary_path, final_path)) {
      setState(
          State::ActionRequired,
          tr("Your map is safe, but its handoff copy could not be preserved."));
      return;
    }
    QFile::remove(temporary_path);
  }
  staged_path = final_path;
  staged_sha256 = sha256;
  staged_revision = snapshot_revision;
  QString working_copy_error;
  if (!commitWorkingCopy(final_path, size_bytes, &working_copy_error)) {
    setState(State::SavedLocally,
             working_copy_error.isEmpty()
                 ? tr("Saved on this device — finishing the open file will retry")
                 : working_copy_error);
    retry_timer->start(retry_interval_ms);
    return;
  }
  if (workspace.file_sha256 == sha256 && !workspace.file_version_id.isEmpty()) {
    setState(State::Synced, tr("Available on your other devices"));
    return;
  }
  uploadStagedFile(final_path, sha256, size_bytes, snapshot_revision);
}

void MapHubSyncController::uploadStagedFile(const QString &path,
                                            const QString &sha256,
                                            qint64 size_bytes,
                                            quint64 map_revision) {
  const auto credential = MapHubCredentials::readToken(workspace.server_url);
  if (!credential || credential.token.isEmpty()) {
    setState(State::WaitingForNetwork,
             tr("Saved on this device — reconnect Map Hub to share it"));
    return;
  }
  request_client =
      new MapHubApiClient(workspace.server_url, credential.token, this);
  upload_pending = true;
  setState(State::Syncing,
           tr("Making this map available on your other devices…"));
  const auto expected_workspace_id = workspace.workspace_id;
  request_client->uploadWorkspaceFile(
      workspace.workspace_id, path, workspace.file_version_id, sha256,
      workspace.client_instance_id, QSysInfo::machineHostName(),
      uploadKey(workspace.workspace_id, workspace.file_version_id, sha256),
      [this, expected_workspace_id, sha256, size_bytes, map_revision](
          const QJsonObject &response, const MapHubApiClient::Error &error) {
        upload_pending = false;
        if (request_client) {
          request_client->deleteLater();
          request_client = nullptr;
        }
        if (workspace.workspace_id != expected_workspace_id)
          return;
        if (error) {
          if (error.code == QLatin1String("file_version_conflict")) {
            stopped_for_conflict = true;
            workspace.sync_problem = error.code;
            saveWorkspace();
            const auto message = tr("Your edits and a newer map are both safe. "
                                    "Reopen Map Hub to compare them.");
            setState(State::UpstreamChanged, message);
            emit upstreamChangeDetected(message);
          } else if (retryable(error)) {
            setState(State::WaitingForNetwork,
                     tr("Saved on this device — waiting for connection"));
            retry_timer->start(retry_interval_ms);
          } else {
            setState(State::ActionRequired,
                     error.message.isEmpty()
                         ? tr("Your map is safe on this device. Open Map Hub "
                              "to continue.")
                         : error.message);
          }
          return;
        }
        const auto current =
            response.value(QStringLiteral("current")).toObject();
        const auto version_id = current.value(QStringLiteral("id")).toString();
        const auto returned_sha =
            current.value(QStringLiteral("sha256")).toString().toLower();
        if (QUuid(version_id).isNull() || returned_sha != sha256) {
          setState(State::ActionRequired,
                   tr("Your map is safe on this device, but Map Hub did not "
                      "confirm the same file."));
          return;
        }
        workspace.file_protocol = QStringLiteral("omap-snapshot/1");
        workspace.file_version_id = version_id;
        workspace.file_generation =
            current.value(QStringLiteral("generation")).toInteger();
        workspace.file_sha256 = returned_sha;
        workspace.file_size_bytes = size_bytes;
        workspace.file_download_url =
            current.value(QStringLiteral("download_url")).toString();
        workspace.last_synced_at = QDateTime::currentDateTimeUtc();
        workspace.sync_problem.clear();
        staged_revision = map_revision;
        saveWorkspace();
        setState(State::Synced, tr("Available on your other devices"));
      });
}

void MapHubSyncController::pollFileState() {
  if (!workspace.isValid() || poll_pending || upload_pending ||
      stopped_for_conflict)
    return;
  const auto credential = MapHubCredentials::readToken(workspace.server_url);
  if (!credential || credential.token.isEmpty())
    return;
  auto *client =
      new MapHubApiClient(workspace.server_url, credential.token, this);
  poll_pending = true;
  const auto expected_workspace_id = workspace.workspace_id;
  client->workspaceFiles(
      workspace.workspace_id, workspace.file_etag, workspace.client_instance_id,
      [this, client, expected_workspace_id](
          const QJsonObject &response, const QString &etag, bool not_modified,
          const MapHubApiClient::Error &error) {
        poll_pending = false;
        client->deleteLater();
        if (workspace.workspace_id != expected_workspace_id)
          return;
        if (error) {
          if (retryable(error))
            setState(State::WaitingForNetwork,
                     tr("Saved on this device — waiting for connection"));
          else
            setState(State::ActionRequired, error.message);
          return;
        }
        if (!etag.isEmpty())
          workspace.file_etag = etag;
        if (not_modified) {
          saveWorkspace();
          return;
        }
        const auto current =
            response.value(QStringLiteral("current")).toObject();
        const auto remote_id = current.value(QStringLiteral("id")).toString();
        const auto remote_sha =
            current.value(QStringLiteral("sha256")).toString().toLower();
        if (remote_id.isEmpty()) {
          saveWorkspace();
          if (workspace.file_version_id.isEmpty())
            stage_timer->start(0);
          return;
        }
        if (remote_id == workspace.file_version_id) {
          saveWorkspace();
          if (staged_sha256.isEmpty() || staged_sha256 == remote_sha)
            setState(State::Synced, tr("Available on your other devices"));
          return;
        }
        QString local_error;
        const auto local_sha = MapHubApiClient::sha256ForFile(
                                   workspace.local_map_path, &local_error)
                                   .toLower();
        const auto editor_unchanged =
            !revision_provider || revision_provider() == staged_revision;
        if (editor_unchanged && local_sha == remote_sha) {
          workspace.file_version_id = remote_id;
          workspace.file_generation =
              current.value(QStringLiteral("generation")).toInteger();
          workspace.file_sha256 = remote_sha;
          workspace.file_size_bytes =
              current.value(QStringLiteral("size_bytes")).toInteger();
          workspace.file_download_url =
              current.value(QStringLiteral("download_url")).toString();
          workspace.sync_problem.clear();
          saveWorkspace();
          setState(State::Synced, tr("Available on your other devices"));
          return;
        }
        stopped_for_conflict = true;
        workspace.sync_problem = QStringLiteral("newer_file_available");
        saveWorkspace();
        const auto message =
            tr("A newer saved map is available. Your open "
               "file is unchanged; reopen Map Hub to compare.");
        setState(State::UpstreamChanged, message);
        emit upstreamChangeDetected(message);
      });
}

bool MapHubSyncController::checkpointFileVersion(QString *version_id) {
  if (upload_pending || stopped_for_conflict ||
      workspace.file_version_id.isEmpty())
    return false;
  QString error;
  const auto local_sha =
      MapHubApiClient::sha256ForFile(workspace.local_map_path, &error)
          .toLower();
  if (local_sha.isEmpty() || local_sha != workspace.file_sha256)
    return false;
  if (version_id)
    *version_id = workspace.file_version_id;
  return true;
}

void MapHubSyncController::setState(State state, const QString &text) {
  if (current_state == state && state_text == text)
    return;
  current_state = state;
  state_text = text;
  emit stateChanged(state, text);
}

bool MapHubSyncController::editable() const {
  return workspace.isValid() &&
         workspace.status != QLatin1String("submitted") &&
         workspace.status != QLatin1String("complete") &&
         workspace.status != QLatin1String("cancelled");
}

bool MapHubSyncController::saveWorkspace() {
  QString error;
  if (ManagedMapWorkspace::save(workspace, &error))
    return true;
  setState(
      State::ActionRequired,
      error.isEmpty()
          ? tr("Your map is safe, but its Map Hub status could not be saved.")
          : error);
  return false;
}

bool MapHubSyncController::commitWorkingCopy(const QString &snapshot_path,
                                             qint64 expected_size,
                                             QString *error) {
  if (error)
    error->clear();
  if (working_copy_committer)
    return working_copy_committer(snapshot_path, expected_size, error);
  const auto committed = copyFileAtomically(
      snapshot_path, workspace.local_map_path, expected_size);
  if (!committed && error)
    *error = tr("Your map is safe, but Mapper could not finish saving the open .omap file.");
  return committed;
}

} // namespace OpenOrienteering
