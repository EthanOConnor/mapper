/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MANAGED_MAP_WORKSPACE_H
#define OPENORIENTEERING_MANAGED_MAP_WORKSPACE_H

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace OpenOrienteering {

/**
 * Local binding between an ordinary .omap file and a Map Hub workspace.
 *
 * The record lives under AppDataLocation, not in the map document. It stores
 * server IDs and synchronization state only. API and lease bearer secrets are
 * kept by MapHubCredentials.
 */
struct ManagedMapWorkspace {
  static constexpr int current_schema_version = 1;

  int schema_version = current_schema_version;
  QString local_map_path;
  QString server_url;
  QString organization_id;
  QString organization_name;
  QString project_id;
  QString project_title;
  QString target_crs;
  int target_scale = 0;
  QString symbol_standard;
  QString work_package_id;
  QString workspace_id;
  QString assignment_id;
  QString manifest_url;
  QString source_artifact_path;
  QString base_artifact_kind;
  QString base_artifact_name;
  QString base_revision_id;
  int base_revision_number = 0;
  QString base_sha256;
  QString active_revision_id;
  int active_revision_number = 0;
  QString active_sha256;
  QString project_revision_id;
  QString sync_etag;
  QString sync_problem;
  QString stream_protocol;
  bool initial_snapshot_required = false;
  qint64 uncompacted_operations = 0;
  bool compaction_recommended = false;
  bool compaction_required = false;
  qint64 stream_head_sequence = 0;
  QString stream_head_hash;
  qint64 minimum_available_sequence = 0;
  qint64 applied_stream_sequence = 0;
  QString client_instance_id;
  qint64 acknowledged_client_sequence = 0;
  qint64 snapshot_stream_sequence = 0;
  QString snapshot_stream_hash;
  QString snapshot_id;
  QString snapshot_sha256;
  qint64 snapshot_size_bytes = 0;
  QString snapshot_download_url;
  QString snapshot_entity_index_sha256;
  QString snapshot_entity_index_download_url;
  QString snapshot_revision_id;
  QString status;
  bool exclusive_editing = false;
  QDateTime lease_expires_at;
  QDateTime last_synced_at;

  bool isValid() const;
  QJsonObject toJson() const;
  static ManagedMapWorkspace fromJson(const QJsonObject &object,
                                      QString *error = nullptr);

  static QString recordPathForMap(const QString &local_map_path);
  static bool save(const ManagedMapWorkspace &workspace,
                   QString *error = nullptr);
  static ManagedMapWorkspace loadForMap(const QString &local_map_path,
                                        QString *error = nullptr);
  static ManagedMapWorkspace findForWorkspace(const QString &server_url,
                                              const QString &workspace_id,
                                              QString *error = nullptr);
  static bool removeForMap(const QString &local_map_path,
                           QString *error = nullptr);
};

} // namespace OpenOrienteering

#endif
