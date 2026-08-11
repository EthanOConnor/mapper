/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "managed_map_workspace.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include "core/document_path.h"
#include "map_hub_api_client.h"

namespace OpenOrienteering {

namespace {

QString canonicalMapPath(const QString &path) {
  return DocumentPath::canonical(path);
}

QString dateString(const QDateTime &value) {
  return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs)
                         : QString{};
}

QDateTime parseDate(const QJsonValue &value) {
  return QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
}

} // namespace

bool ManagedMapWorkspace::isValid() const {
  return schema_version == current_schema_version &&
         !local_map_path.isEmpty() && !server_url.isEmpty() &&
         !project_id.isEmpty() && !work_package_id.isEmpty() &&
         !workspace_id.isEmpty();
}

QJsonObject ManagedMapWorkspace::toJson() const {
  return {
      {QStringLiteral("schema_version"), schema_version},
      {QStringLiteral("local_map_path"), canonicalMapPath(local_map_path)},
      {QStringLiteral("server_url"), server_url},
      {QStringLiteral("organization_id"), organization_id},
      {QStringLiteral("organization_name"), organization_name},
      {QStringLiteral("project_id"), project_id},
      {QStringLiteral("project_title"), project_title},
      {QStringLiteral("target_crs"), target_crs},
      {QStringLiteral("target_scale"), target_scale},
      {QStringLiteral("symbol_standard"), symbol_standard},
      {QStringLiteral("work_package_id"), work_package_id},
      {QStringLiteral("workspace_id"), workspace_id},
      {QStringLiteral("assignment_id"), assignment_id},
      {QStringLiteral("manifest_url"), manifest_url},
      {QStringLiteral("source_artifact_path"),
       source_artifact_path.isEmpty() ? QString{}
                                      : canonicalMapPath(source_artifact_path)},
      {QStringLiteral("base_artifact_kind"), base_artifact_kind},
      {QStringLiteral("base_artifact_name"), base_artifact_name},
      {QStringLiteral("base_revision_id"), base_revision_id},
      {QStringLiteral("base_revision_number"), base_revision_number},
      {QStringLiteral("base_sha256"), base_sha256},
      {QStringLiteral("active_revision_id"), active_revision_id},
      {QStringLiteral("active_revision_number"), active_revision_number},
      {QStringLiteral("active_sha256"), active_sha256},
      {QStringLiteral("project_revision_id"), project_revision_id},
      {QStringLiteral("sync_etag"), sync_etag},
      {QStringLiteral("sync_problem"), sync_problem},
      {QStringLiteral("checkpoint_required"), checkpoint_required},
      {QStringLiteral("stream_protocol"), stream_protocol},
      {QStringLiteral("initial_snapshot_required"),
       initial_snapshot_required},
      {QStringLiteral("uncompacted_operations"),
       QString::number(uncompacted_operations)},
      {QStringLiteral("compaction_recommended"),
       compaction_recommended},
      {QStringLiteral("compaction_required"), compaction_required},
      {QStringLiteral("stream_head_sequence"),
       QString::number(stream_head_sequence)},
      {QStringLiteral("stream_head_hash"), stream_head_hash},
      {QStringLiteral("minimum_available_sequence"),
       QString::number(minimum_available_sequence)},
      {QStringLiteral("applied_stream_sequence"),
       QString::number(applied_stream_sequence)},
      {QStringLiteral("client_instance_id"), client_instance_id},
      {QStringLiteral("acknowledged_client_sequence"),
       QString::number(acknowledged_client_sequence)},
      {QStringLiteral("snapshot_stream_sequence"),
       QString::number(snapshot_stream_sequence)},
      {QStringLiteral("snapshot_stream_hash"), snapshot_stream_hash},
      {QStringLiteral("snapshot_id"), snapshot_id},
      {QStringLiteral("snapshot_sha256"), snapshot_sha256},
      {QStringLiteral("snapshot_size_bytes"),
       QString::number(snapshot_size_bytes)},
      {QStringLiteral("snapshot_download_url"), snapshot_download_url},
      {QStringLiteral("snapshot_entity_index_sha256"),
       snapshot_entity_index_sha256},
      {QStringLiteral("snapshot_entity_index_download_url"),
       snapshot_entity_index_download_url},
      {QStringLiteral("snapshot_revision_id"), snapshot_revision_id},
      {QStringLiteral("status"), status},
      {QStringLiteral("exclusive_editing"), exclusive_editing},
      {QStringLiteral("lease_expires_at"), dateString(lease_expires_at)},
      {QStringLiteral("last_synced_at"), dateString(last_synced_at)},
  };
}

ManagedMapWorkspace ManagedMapWorkspace::fromJson(const QJsonObject &object,
                                                  QString *error) {
  ManagedMapWorkspace workspace;
  workspace.schema_version =
      object.value(QStringLiteral("schema_version")).toInt();
  workspace.local_map_path =
      object.value(QStringLiteral("local_map_path")).toString();
  workspace.server_url = object.value(QStringLiteral("server_url")).toString();
  workspace.organization_id =
      object.value(QStringLiteral("organization_id")).toString();
  workspace.organization_name =
      object.value(QStringLiteral("organization_name")).toString();
  workspace.project_id = object.value(QStringLiteral("project_id")).toString();
  workspace.project_title =
      object.value(QStringLiteral("project_title")).toString();
  workspace.target_crs = object.value(QStringLiteral("target_crs")).toString();
  workspace.target_scale = object.value(QStringLiteral("target_scale")).toInt();
  workspace.symbol_standard =
      object.value(QStringLiteral("symbol_standard")).toString();
  workspace.work_package_id =
      object.value(QStringLiteral("work_package_id")).toString();
  workspace.workspace_id =
      object.value(QStringLiteral("workspace_id")).toString();
  workspace.assignment_id =
      object.value(QStringLiteral("assignment_id")).toString();
  workspace.manifest_url =
      object.value(QStringLiteral("manifest_url")).toString();
  workspace.source_artifact_path =
      object.value(QStringLiteral("source_artifact_path")).toString();
  workspace.base_artifact_kind =
      object.value(QStringLiteral("base_artifact_kind")).toString();
  workspace.base_artifact_name =
      object.value(QStringLiteral("base_artifact_name")).toString();
  workspace.base_revision_id =
      object.value(QStringLiteral("base_revision_id")).toString();
  workspace.base_revision_number =
      object.value(QStringLiteral("base_revision_number")).toInt();
  workspace.base_sha256 =
      object.value(QStringLiteral("base_sha256")).toString();
  workspace.active_revision_id =
      object.value(QStringLiteral("active_revision_id")).toString();
  workspace.active_revision_number =
      object.value(QStringLiteral("active_revision_number")).toInt();
  workspace.active_sha256 =
      object.value(QStringLiteral("active_sha256")).toString();
  workspace.project_revision_id =
      object.value(QStringLiteral("project_revision_id")).toString();
  workspace.sync_etag =
      object.value(QStringLiteral("sync_etag")).toString();
  workspace.sync_problem =
      object.value(QStringLiteral("sync_problem")).toString();
  workspace.checkpoint_required =
      object.value(QStringLiteral("checkpoint_required")).toBool();
  workspace.stream_protocol =
      object.value(QStringLiteral("stream_protocol")).toString();
  workspace.initial_snapshot_required =
      object.value(QStringLiteral("initial_snapshot_required")).toBool();
  workspace.uncompacted_operations =
      object.value(QStringLiteral("uncompacted_operations"))
          .toString()
          .toLongLong();
  workspace.compaction_recommended =
      object.value(QStringLiteral("compaction_recommended")).toBool();
  workspace.compaction_required =
      object.value(QStringLiteral("compaction_required")).toBool();
  workspace.stream_head_sequence =
      object.value(QStringLiteral("stream_head_sequence")).toString().toLongLong();
  workspace.stream_head_hash =
      object.value(QStringLiteral("stream_head_hash")).toString();
  workspace.minimum_available_sequence =
      object.value(QStringLiteral("minimum_available_sequence"))
          .toString()
          .toLongLong();
  workspace.applied_stream_sequence =
      object.value(QStringLiteral("applied_stream_sequence"))
          .toString()
          .toLongLong();
  workspace.client_instance_id =
      object.value(QStringLiteral("client_instance_id")).toString();
  workspace.acknowledged_client_sequence =
      object.value(QStringLiteral("acknowledged_client_sequence"))
          .toString()
          .toLongLong();
  workspace.snapshot_stream_sequence =
      object.value(QStringLiteral("snapshot_stream_sequence"))
          .toString()
          .toLongLong();
  workspace.snapshot_stream_hash =
      object.value(QStringLiteral("snapshot_stream_hash")).toString();
  workspace.snapshot_id =
      object.value(QStringLiteral("snapshot_id")).toString();
  workspace.snapshot_sha256 =
      object.value(QStringLiteral("snapshot_sha256")).toString();
  workspace.snapshot_size_bytes =
      object.value(QStringLiteral("snapshot_size_bytes"))
          .toString()
          .toLongLong();
  workspace.snapshot_download_url =
      object.value(QStringLiteral("snapshot_download_url")).toString();
  workspace.snapshot_entity_index_sha256 =
      object.value(QStringLiteral("snapshot_entity_index_sha256")).toString();
  workspace.snapshot_entity_index_download_url =
      object.value(QStringLiteral("snapshot_entity_index_download_url"))
          .toString();
  workspace.snapshot_revision_id =
      object.value(QStringLiteral("snapshot_revision_id")).toString();
  workspace.status = object.value(QStringLiteral("status")).toString();
  workspace.exclusive_editing =
      object.value(QStringLiteral("exclusive_editing")).toBool();
  workspace.lease_expires_at =
      parseDate(object.value(QStringLiteral("lease_expires_at")));
  workspace.last_synced_at =
      parseDate(object.value(QStringLiteral("last_synced_at")));
  if (!workspace.isValid() && error)
    *error = QStringLiteral("The managed workspace record is incomplete or "
                            "uses an unsupported schema version.");
  return workspace;
}

QString ManagedMapWorkspace::recordPathForMap(const QString &local_map_path) {
  auto identity = canonicalMapPath(local_map_path);
  auto digest =
      QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256)
          .toHex();
  auto root = qEnvironmentVariable("MAPPER_MANAGED_WORKSPACE_ROOT");
  if (root.isEmpty())
    root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return QDir(root).filePath(QStringLiteral("managed-workspaces/%1.json")
                                 .arg(QString::fromLatin1(digest)));
}

bool ManagedMapWorkspace::save(const ManagedMapWorkspace &workspace,
                               QString *error) {
  if (!workspace.isValid()) {
    if (error)
      *error = QStringLiteral("The managed workspace record is incomplete.");
    return false;
  }
  auto path = recordPathForMap(workspace.local_map_path);
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    if (error)
      *error = QStringLiteral("Cannot create the managed workspace directory.");
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    if (error)
      *error = file.errorString();
    return false;
  }
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  if (file.write(
          QJsonDocument(workspace.toJson()).toJson(QJsonDocument::Indented)) <
          0 ||
      !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return true;
}

ManagedMapWorkspace
ManagedMapWorkspace::loadForMap(const QString &local_map_path, QString *error) {
  QFile file(recordPathForMap(local_map_path));
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
  auto workspace = fromJson(document.object(), error);
  if (workspace.isValid() && canonicalMapPath(workspace.local_map_path) !=
                                 canonicalMapPath(local_map_path)) {
    if (error)
      *error = QStringLiteral("The managed workspace record belongs to a "
                              "different local map path.");
    return {};
  }
  return workspace;
}

ManagedMapWorkspace ManagedMapWorkspace::findForWorkspace(
    const QString &server_url, const QString &workspace_id, QString *error) {
  const auto canonical_server =
      MapHubApiClient::canonicalServerOrigin(server_url);
  if (canonical_server.isEmpty() || workspace_id.isEmpty()) {
    if (error)
      *error = QStringLiteral("The server or workspace identity is invalid.");
    return {};
  }
  auto root = qEnvironmentVariable("MAPPER_MANAGED_WORKSPACE_ROOT");
  if (root.isEmpty())
    root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  const QDir directory(
      QDir(root).filePath(QStringLiteral("managed-workspaces")));
  ManagedMapWorkspace best;
  QDateTime best_time;
  for (const auto &name :
       directory.entryList({QStringLiteral("*.json")}, QDir::Files)) {
    QFile file(directory.filePath(name));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
      continue;
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
      continue;
    QString record_error;
    const auto candidate = fromJson(document.object(), &record_error);
    if (!candidate.isValid() ||
        MapHubApiClient::canonicalServerOrigin(candidate.server_url) !=
            canonical_server ||
        candidate.workspace_id != workspace_id ||
        !QFileInfo::exists(candidate.local_map_path))
      continue;
    const auto candidate_time =
        candidate.last_synced_at.isValid()
            ? candidate.last_synced_at
            : QFileInfo(candidate.local_map_path).lastModified();
    if (!best.isValid() || candidate_time > best_time) {
      best = candidate;
      best_time = candidate_time;
    }
  }
  if (!best.isValid() && error)
    *error = QStringLiteral("No existing local workspace was found.");
  return best;
}

ManagedMapWorkspace ManagedMapWorkspace::findForAssignment(
    const QString &server_url, const QString &assignment_id, QString *error) {
  const auto canonical_server =
      MapHubApiClient::canonicalServerOrigin(server_url);
  if (canonical_server.isEmpty() || assignment_id.isEmpty()) {
    if (error)
      *error = QStringLiteral("The server or assignment identity is empty.");
    return {};
  }

  auto root = qEnvironmentVariable("MAPPER_MANAGED_WORKSPACE_ROOT");
  if (root.isEmpty())
    root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  const QDir directory(
      QDir(root).filePath(QStringLiteral("managed-workspaces")));
  ManagedMapWorkspace best;
  QDateTime best_time;
  for (const auto &name :
       directory.entryList({QStringLiteral("*.json")}, QDir::Files)) {
    QFile file(directory.filePath(name));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024)
      continue;
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject())
      continue;
    QString record_error;
    const auto candidate = fromJson(document.object(), &record_error);
    const auto candidate_server =
        MapHubApiClient::canonicalServerOrigin(candidate.server_url);
    const QFileInfo local_file(candidate.local_map_path);
    const QFileInfo expected_record(recordPathForMap(candidate.local_map_path));
    const QFileInfo scanned_record(file.fileName());
    if (!candidate.isValid() || candidate_server != canonical_server ||
        candidate.assignment_id != assignment_id || !local_file.exists() ||
        !local_file.isFile() ||
        expected_record.absoluteFilePath() != scanned_record.absoluteFilePath())
      continue;
    const auto candidate_time =
        candidate.last_synced_at.isValid() ? candidate.last_synced_at
                                           : local_file.lastModified();
    if (!best.isValid() || candidate_time > best_time) {
      best = candidate;
      best_time = candidate_time;
    }
  }
  if (!best.isValid() && error)
    *error = QStringLiteral("No existing local assignment was found.");
  return best;
}

bool ManagedMapWorkspace::removeForMap(const QString &local_map_path,
                                       QString *error) {
  QFile file(recordPathForMap(local_map_path));
  if (!file.exists() || file.remove())
    return true;
  if (error)
    *error = file.errorString();
  return false;
}

} // namespace OpenOrienteering
