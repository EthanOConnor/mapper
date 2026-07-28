/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_read_only_document.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>

#include "collaboration/map_hub_api_client.h"
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

bool validSha256(const QString &value) {
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return pattern.match(value).hasMatch();
}

bool validCanonicalUuid(const QString &value) {
  const QUuid uuid(value);
  return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == value;
}

} // namespace

bool MapHubReadOnlyDocument::isValid() const {
  return schema_version == current_schema_version &&
         !local_map_path.isEmpty() &&
         MapHubApiClient::isAcceptableServerUrl(
             QUrl::fromUserInput(server_url)) &&
         validCanonicalUuid(organization_id) &&
         validCanonicalUuid(project_id) && validCanonicalUuid(revision_id) &&
         revision_number > 0 && validSha256(revision_sha256) &&
         revision_size_bytes > 0 && !artifact_kind.isEmpty() &&
         !artifact_name.isEmpty() &&
         (access_request_id.isEmpty() ||
          validCanonicalUuid(access_request_id)) &&
         (approved_assignment_id.isEmpty() ||
          validCanonicalUuid(approved_assignment_id));
}

QJsonObject MapHubReadOnlyDocument::toJson() const {
  return {
      {QStringLiteral("schema_version"), schema_version},
      {QStringLiteral("local_map_path"), canonicalMapPath(local_map_path)},
      {QStringLiteral("server_url"), server_url},
      {QStringLiteral("organization_id"), organization_id},
      {QStringLiteral("organization_name"), organization_name},
      {QStringLiteral("project_id"), project_id},
      {QStringLiteral("project_title"), project_title},
      {QStringLiteral("revision_id"), revision_id},
      {QStringLiteral("revision_number"), revision_number},
      {QStringLiteral("revision_sha256"), revision_sha256},
      {QStringLiteral("revision_size_bytes"),
       QString::number(revision_size_bytes)},
      {QStringLiteral("artifact_kind"), artifact_kind},
      {QStringLiteral("artifact_name"), artifact_name},
      {QStringLiteral("manifest_url"), manifest_url},
      {QStringLiteral("access_request_id"), access_request_id},
      {QStringLiteral("access_request_status"), access_request_status},
      {QStringLiteral("approved_assignment_id"), approved_assignment_id},
      {QStringLiteral("last_checked_at"), dateString(last_checked_at)},
  };
}

MapHubReadOnlyDocument
MapHubReadOnlyDocument::fromJson(const QJsonObject &object, QString *error) {
  MapHubReadOnlyDocument document;
  document.schema_version =
      object.value(QStringLiteral("schema_version")).toInt();
  document.local_map_path =
      object.value(QStringLiteral("local_map_path")).toString();
  document.server_url = object.value(QStringLiteral("server_url")).toString();
  document.organization_id =
      object.value(QStringLiteral("organization_id")).toString();
  document.organization_name =
      object.value(QStringLiteral("organization_name")).toString();
  document.project_id = object.value(QStringLiteral("project_id")).toString();
  document.project_title =
      object.value(QStringLiteral("project_title")).toString();
  document.revision_id = object.value(QStringLiteral("revision_id")).toString();
  document.revision_number =
      object.value(QStringLiteral("revision_number")).toInt();
  document.revision_sha256 =
      object.value(QStringLiteral("revision_sha256")).toString();
  document.revision_size_bytes =
      object.value(QStringLiteral("revision_size_bytes"))
          .toString()
          .toLongLong();
  document.artifact_kind =
      object.value(QStringLiteral("artifact_kind")).toString();
  document.artifact_name =
      object.value(QStringLiteral("artifact_name")).toString();
  document.manifest_url =
      object.value(QStringLiteral("manifest_url")).toString();
  document.access_request_id =
      object.value(QStringLiteral("access_request_id")).toString();
  document.access_request_status =
      object.value(QStringLiteral("access_request_status")).toString();
  document.approved_assignment_id =
      object.value(QStringLiteral("approved_assignment_id")).toString();
  document.last_checked_at =
      parseDate(object.value(QStringLiteral("last_checked_at")));
  if (!document.isValid() && error)
    *error = QStringLiteral(
        "The Map Hub read-only record is incomplete or unsupported.");
  return document;
}

QString
MapHubReadOnlyDocument::recordPathForMap(const QString &local_map_path) {
  const auto identity = canonicalMapPath(local_map_path);
  const auto digest =
      QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256)
          .toHex();
  auto root = qEnvironmentVariable("MAPPER_MAP_HUB_READ_ONLY_ROOT");
  if (root.isEmpty())
    root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  return QDir(root).filePath(QStringLiteral("map-hub-read-only/%1.json")
                                 .arg(QString::fromLatin1(digest)));
}

bool MapHubReadOnlyDocument::save(const MapHubReadOnlyDocument &document,
                                  QString *error) {
  if (!document.isValid()) {
    if (error)
      *error = QStringLiteral("The Map Hub read-only record is incomplete.");
    return false;
  }
  const auto path = recordPathForMap(document.local_map_path);
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
    if (error)
      *error = QStringLiteral(
          "Cannot create the Map Hub read-only metadata directory.");
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
          QJsonDocument(document.toJson()).toJson(QJsonDocument::Indented)) <
          0 ||
      !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return true;
}

MapHubReadOnlyDocument
MapHubReadOnlyDocument::loadForMap(const QString &local_map_path,
                                   QString *error) {
  QFile file(recordPathForMap(local_map_path));
  if (!file.exists())
    return {};
  if (!file.open(QIODevice::ReadOnly)) {
    if (error)
      *error = file.errorString();
    return {};
  }
  if (file.size() > 1024 * 1024) {
    if (error)
      *error = QStringLiteral("The Map Hub read-only record is too large.");
    return {};
  }
  QJsonParseError parse_error;
  const auto document = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    if (error)
      *error = parse_error.errorString();
    return {};
  }
  auto result = fromJson(document.object(), error);
  if (result.isValid() && canonicalMapPath(result.local_map_path) !=
                              canonicalMapPath(local_map_path)) {
    if (error)
      *error = QStringLiteral(
          "The Map Hub read-only record belongs to another map path.");
    return {};
  }
  return result;
}

bool MapHubReadOnlyDocument::removeForMap(const QString &local_map_path,
                                          QString *error) {
  QFile file(recordPathForMap(local_map_path));
  if (!file.exists() || file.remove())
    return true;
  if (error)
    *error = file.errorString();
  return false;
}

} // namespace OpenOrienteering
