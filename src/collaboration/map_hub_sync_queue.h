/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_SYNC_QUEUE_H
#define OPENORIENTEERING_MAP_HUB_SYNC_QUEUE_H

#include <QDateTime>
#include <QString>

namespace OpenOrienteering {

/**
 * Durable description of the newest complete local snapshot awaiting Map Hub.
 *
 * Snapshot bytes are immutable and content-addressed. Replacing this record
 * coalesces pending work without invalidating a QFile already being uploaded.
 */
struct MapHubPendingDraft {
  static constexpr int current_schema_version = 1;

  int schema_version = current_schema_version;
  QString workspace_id;
  QString local_map_path;
  QString snapshot_path;
  QString sha256;
  qint64 size_bytes = 0;
  QString entity_index_path;
  QString entity_index_sha256;
  qint64 base_stream_sequence = 0;
  QString base_stream_hash;
  QString client_instance_id;
  bool bootstrap = false;
  bool publish_snapshot = false;
  quint64 map_revision = 0;
  QString expected_workspace_revision_id;
  QString expected_project_revision_id;
  QString idempotency_key;
  QDateTime staged_at;
  int attempt_count = 0;
  QDateTime next_attempt_at;
  QString last_error_code;
  QString last_error_message;

  bool isValid() const;
};

class MapHubSyncQueue {
public:
  static QString rootPath();
  static QString workspaceDirectory(const QString &workspace_id);
  static QString snapshotPath(const QString &workspace_id,
                              const QString &sha256);
  static QString entityIndexPath(const QString &workspace_id,
                                 const QString &sha256);
  static QString recordPath(const QString &workspace_id);

  static bool save(const MapHubPendingDraft &draft, QString *error = nullptr);
  static MapHubPendingDraft load(const QString &workspace_id,
                                 QString *error = nullptr);
  static bool remove(const QString &workspace_id, QString *error = nullptr);

  /**
   * Removes content-addressed snapshots other than keep_snapshot_path.
   * A currently open upload remains valid on Unix-like platforms even when its
   * old directory entry is removed, but callers should invoke this only after
   * the corresponding network reply has finished.
   */
  static bool pruneSnapshots(const QString &workspace_id,
                             const QString &keep_snapshot_path = {},
                             QString *error = nullptr,
                             const QString &keep_entity_index_path = {});

  static QString idempotencyKey(const QString &workspace_id,
                                const QString &expected_workspace_revision_id,
                                const QString &sha256,
                                qint64 base_stream_sequence = -1,
                                const QString &base_stream_hash = {},
                                const QString &entity_index_sha256 = {});
};

} // namespace OpenOrienteering

#endif
