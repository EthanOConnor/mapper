/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_READ_ONLY_DOCUMENT_H
#define OPENORIENTEERING_MAP_HUB_READ_ONLY_DOCUMENT_H

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace OpenOrienteering {

/**
 * Durable provenance for an immutable Map Hub library revision opened in
 * Mapper's viewer mode.
 *
 * Account and lease credentials deliberately remain in the platform credential
 * store.  This record is only the authority needed to keep a cached revision
 * read-only across app restarts and to resume an edit-access request.
 */
struct MapHubReadOnlyDocument {
  static constexpr int current_schema_version = 1;

  int schema_version = current_schema_version;
  QString local_map_path;
  QString server_url;
  QString organization_id;
  QString organization_name;
  QString project_id;
  QString project_title;
  QString revision_id;
  int revision_number = 0;
  QString revision_sha256;
  qint64 revision_size_bytes = 0;
  QString artifact_kind;
  QString artifact_name;
  QString manifest_url;
  QString access_request_id;
  QString access_request_status;
  QString approved_assignment_id;
  QDateTime last_checked_at;

  bool isValid() const;
  QJsonObject toJson() const;

  static MapHubReadOnlyDocument fromJson(const QJsonObject &object,
                                         QString *error = nullptr);
  static QString recordPathForMap(const QString &local_map_path);
  static bool save(const MapHubReadOnlyDocument &document,
                   QString *error = nullptr);
  static MapHubReadOnlyDocument loadForMap(const QString &local_map_path,
                                           QString *error = nullptr);
  static bool removeForMap(const QString &local_map_path,
                           QString *error = nullptr);
};

} // namespace OpenOrienteering

#endif
