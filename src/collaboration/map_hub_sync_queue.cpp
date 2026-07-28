/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_sync_queue.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include "core/document_path.h"

namespace OpenOrienteering {

namespace {

QString dateString(const QDateTime &value) {
  return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs)
                         : QString{};
}

QDateTime parseDate(const QJsonValue &value) {
  return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

bool validSha256(const QString &value) {
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return pattern.match(value).hasMatch();
}

QString canonicalPath(const QString &path) {
  return DocumentPath::canonical(path);
}

} // namespace

bool MapHubPendingDraft::isValid() const {
  const auto valid_index =
      !publish_snapshot ||
      (!entity_index_path.isEmpty() && validSha256(entity_index_sha256) &&
       validSha256(base_stream_hash) && !QUuid(client_instance_id).isNull());
  return schema_version == current_schema_version &&
         !QUuid(workspace_id).isNull() && !local_map_path.isEmpty() &&
         !snapshot_path.isEmpty() && validSha256(sha256) && size_bytes > 0 &&
         map_revision > 0 && valid_index && !idempotency_key.isEmpty() &&
         idempotency_key.size() <= 120 && staged_at.isValid();
}

QString MapHubSyncQueue::rootPath() {
  auto root = qEnvironmentVariable("MAPPER_MAP_HUB_SYNC_ROOT");
  if (root.isEmpty())
    root =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("map-hub-sync"));
  return QDir::cleanPath(root);
}

QString MapHubSyncQueue::workspaceDirectory(const QString &workspace_id) {
  if (QUuid(workspace_id).isNull())
    return {};
  return QDir(rootPath()).filePath(workspace_id.toLower());
}

QString MapHubSyncQueue::snapshotPath(const QString &workspace_id,
                                      const QString &sha256) {
  if (!validSha256(sha256))
    return {};
  auto directory = workspaceDirectory(workspace_id);
  return directory.isEmpty()
             ? QString{}
             : QDir(directory).filePath(
                   QStringLiteral("snapshot-%1.omap").arg(sha256));
}

QString MapHubSyncQueue::entityIndexPath(const QString &workspace_id,
                                         const QString &sha256) {
  if (!validSha256(sha256))
    return {};
  const auto directory = workspaceDirectory(workspace_id);
  return directory.isEmpty()
             ? QString{}
             : QDir(directory).filePath(
                   QStringLiteral("entity-index-%1.json").arg(sha256));
}

QString MapHubSyncQueue::recordPath(const QString &workspace_id) {
  auto directory = workspaceDirectory(workspace_id);
  return directory.isEmpty()
             ? QString{}
             : QDir(directory).filePath(QStringLiteral("pending.json"));
}

bool MapHubSyncQueue::save(const MapHubPendingDraft &draft, QString *error) {
  if (!draft.isValid() ||
      canonicalPath(draft.snapshot_path) !=
          canonicalPath(snapshotPath(draft.workspace_id, draft.sha256)) ||
      (draft.publish_snapshot &&
       canonicalPath(draft.entity_index_path) !=
           canonicalPath(entityIndexPath(draft.workspace_id,
                                         draft.entity_index_sha256)))) {
    if (error)
      *error =
          QStringLiteral("The pending Map Hub recovery copy is incomplete or "
                         "does not identify its content-addressed file.");
    return false;
  }
  if (!QFileInfo::exists(draft.snapshot_path)) {
    if (error)
      *error = QStringLiteral("The pending Map Hub snapshot does not exist.");
    return false;
  }
  if (draft.publish_snapshot && !QFileInfo::exists(draft.entity_index_path)) {
    if (error)
      *error =
          QStringLiteral("The pending Map Hub entity index does not exist.");
    return false;
  }
  auto path = recordPath(draft.workspace_id);
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    if (error)
      *error = QStringLiteral("Cannot create the Map Hub synchronization "
                              "queue directory.");
    return false;
  }
  QJsonObject object{
      {QStringLiteral("schema_version"), draft.schema_version},
      {QStringLiteral("workspace_id"), draft.workspace_id},
      {QStringLiteral("local_map_path"), canonicalPath(draft.local_map_path)},
      {QStringLiteral("snapshot_path"), canonicalPath(draft.snapshot_path)},
      {QStringLiteral("sha256"), draft.sha256},
      {QStringLiteral("size_bytes"), QString::number(draft.size_bytes)},
      {QStringLiteral("entity_index_path"),
       canonicalPath(draft.entity_index_path)},
      {QStringLiteral("entity_index_sha256"), draft.entity_index_sha256},
      {QStringLiteral("base_stream_sequence"),
       QString::number(draft.base_stream_sequence)},
      {QStringLiteral("base_stream_hash"), draft.base_stream_hash},
      {QStringLiteral("client_instance_id"), draft.client_instance_id},
      {QStringLiteral("bootstrap"), draft.bootstrap},
      {QStringLiteral("publish_snapshot"), draft.publish_snapshot},
      {QStringLiteral("map_revision"), QString::number(draft.map_revision)},
      {QStringLiteral("expected_workspace_revision_id"),
       draft.expected_workspace_revision_id},
      {QStringLiteral("expected_project_revision_id"),
       draft.expected_project_revision_id},
      {QStringLiteral("idempotency_key"), draft.idempotency_key},
      {QStringLiteral("staged_at"), dateString(draft.staged_at)},
      {QStringLiteral("attempt_count"), draft.attempt_count},
      {QStringLiteral("next_attempt_at"), dateString(draft.next_attempt_at)},
      {QStringLiteral("last_error_code"), draft.last_error_code},
      {QStringLiteral("last_error_message"), draft.last_error_message},
  };
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    if (error)
      *error = file.errorString();
    return false;
  }
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  if (file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) < 0 ||
      !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return true;
}

MapHubPendingDraft MapHubSyncQueue::load(const QString &workspace_id,
                                         QString *error) {
  QFile file(recordPath(workspace_id));
  if (!file.exists())
    return {};
  if (!file.open(QIODevice::ReadOnly)) {
    if (error)
      *error = file.errorString();
    return {};
  }
  QJsonParseError parse_error;
  auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    if (error)
      *error = parse_error.errorString();
    return {};
  }
  auto object = document.object();
  MapHubPendingDraft draft;
  draft.schema_version = object.value(QStringLiteral("schema_version")).toInt();
  draft.workspace_id = object.value(QStringLiteral("workspace_id")).toString();
  draft.local_map_path =
      object.value(QStringLiteral("local_map_path")).toString();
  draft.snapshot_path =
      object.value(QStringLiteral("snapshot_path")).toString();
  draft.sha256 = object.value(QStringLiteral("sha256")).toString();
  draft.size_bytes =
      object.value(QStringLiteral("size_bytes")).toString().toLongLong();
  draft.entity_index_path =
      object.value(QStringLiteral("entity_index_path")).toString();
  draft.entity_index_sha256 =
      object.value(QStringLiteral("entity_index_sha256")).toString();
  draft.base_stream_sequence =
      object.value(QStringLiteral("base_stream_sequence"))
          .toString()
          .toLongLong();
  draft.base_stream_hash =
      object.value(QStringLiteral("base_stream_hash")).toString();
  draft.client_instance_id =
      object.value(QStringLiteral("client_instance_id")).toString();
  draft.bootstrap = object.value(QStringLiteral("bootstrap")).toBool();
  draft.publish_snapshot =
      object.value(QStringLiteral("publish_snapshot")).toBool();
  bool revision_ok = false;
  draft.map_revision = object.value(QStringLiteral("map_revision"))
                           .toString()
                           .toULongLong(&revision_ok);
  draft.expected_workspace_revision_id =
      object.value(QStringLiteral("expected_workspace_revision_id")).toString();
  draft.expected_project_revision_id =
      object.value(QStringLiteral("expected_project_revision_id")).toString();
  draft.idempotency_key =
      object.value(QStringLiteral("idempotency_key")).toString();
  draft.staged_at = parseDate(object.value(QStringLiteral("staged_at")));
  draft.attempt_count = object.value(QStringLiteral("attempt_count")).toInt();
  draft.next_attempt_at =
      parseDate(object.value(QStringLiteral("next_attempt_at")));
  draft.last_error_code =
      object.value(QStringLiteral("last_error_code")).toString();
  draft.last_error_message =
      object.value(QStringLiteral("last_error_message")).toString();
  if (!revision_ok || !draft.isValid() ||
      draft.workspace_id.compare(workspace_id, Qt::CaseInsensitive) != 0 ||
      canonicalPath(draft.snapshot_path) !=
          canonicalPath(snapshotPath(draft.workspace_id, draft.sha256)) ||
      (draft.publish_snapshot &&
       canonicalPath(draft.entity_index_path) !=
           canonicalPath(entityIndexPath(draft.workspace_id,
                                         draft.entity_index_sha256))) ||
      !QFileInfo::exists(draft.snapshot_path) ||
      (draft.publish_snapshot && !QFileInfo::exists(draft.entity_index_path))) {
    if (error)
      *error =
          QStringLiteral("The pending Map Hub recovery record is invalid.");
    return {};
  }
  return draft;
}

bool MapHubSyncQueue::remove(const QString &workspace_id, QString *error) {
  QFile record(recordPath(workspace_id));
  if (record.exists() && !record.remove()) {
    if (error)
      *error = record.errorString();
    return false;
  }
  return pruneSnapshots(workspace_id, {}, error);
}

bool MapHubSyncQueue::pruneSnapshots(const QString &workspace_id,
                                     const QString &keep_snapshot_path,
                                     QString *error,
                                     const QString &keep_entity_index_path) {
  QDir directory(workspaceDirectory(workspace_id));
  if (!directory.exists())
    return true;
  auto keep = canonicalPath(keep_snapshot_path);
  const auto keep_index = canonicalPath(keep_entity_index_path);
  for (const auto &name :
       directory.entryList({QStringLiteral("snapshot-*.omap"),
                            QStringLiteral("entity-index-*.json")},
                           QDir::Files)) {
    auto path = canonicalPath(directory.filePath(name));
    if ((!keep.isEmpty() && path == keep) ||
        (!keep_index.isEmpty() && path == keep_index))
      continue;
    QFile file(path);
    if (!file.remove()) {
      if (error)
        *error = file.errorString();
      return false;
    }
  }
  if (directory.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty())
    QDir().rmdir(directory.path());
  return true;
}

QString
MapHubSyncQueue::idempotencyKey(const QString &workspace_id,
                                const QString &expected_workspace_revision_id,
                                const QString &sha256) {
  auto material = workspace_id.toLower() + QLatin1Char('|') +
                  expected_workspace_revision_id.toLower() + QLatin1Char('|') +
                  sha256.toLower();
  auto digest =
      QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha256)
          .toHex()
          .left(48);
  return QStringLiteral("mapper-snapshot-%1").arg(QString::fromLatin1(digest));
}

} // namespace OpenOrienteering
