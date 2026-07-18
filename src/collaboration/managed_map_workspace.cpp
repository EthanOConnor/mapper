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
