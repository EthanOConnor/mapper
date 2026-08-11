/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_SYNC_CONTROLLER_H
#define OPENORIENTEERING_MAP_HUB_SYNC_CONTROLLER_H

#include <functional>
#include <memory>

#include <QHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QVector>

#include "collaboration/managed_map_workspace.h"
#include "collaboration/map_hub_edit_transaction.h"

class QTimer;

namespace OpenOrienteering {

class MapHubApiClient;
class MapHubOperationStore;
class Map;
class UndoStep;

/**
 * Captures editor transactions in a durable local operation log and maintains
 * periodic local recovery snapshots.
 *
 * Immutable checkpoints/submission remain explicit user actions. This class
 * never marks the editor clean.
 */
class MapHubSyncController final : public QObject {
  Q_OBJECT
public:
  enum class State {
    Disconnected,
    Watching,
    SavingLocally,
    SavedLocally,
    Syncing,
    Synced,
    WaitingForNetwork,
    CheckpointNeeded,
    UpstreamChanged,
    ActionRequired,
  };
  Q_ENUM(State)

  using RevisionProvider = std::function<quint64()>;
  using SnapshotProvider = std::function<bool(
      const QString &destination, quint64 *staged_revision, QString *error)>;
  using WorkingCopyCommitter = std::function<bool(
      const QString &snapshot_path, qint64 expected_size, QString *error)>;

  struct Collaborator {
    QString person_id;
    QString client_instance_id;
    QString display_name;
    QDateTime last_seen_at;
    QString state;
    qint64 applied_stream_sequence = 0;
    bool is_current_user = false;
  };

  explicit MapHubSyncController(QObject *parent = nullptr);
  ~MapHubSyncController() override;

  void configure(const ManagedMapWorkspace &workspace, Map *map,
                 RevisionProvider revision_provider,
                 SnapshotProvider snapshot_provider,
                 WorkingCopyCommitter working_copy_committer = {});
  void clear();
  void applicationBecameActive();
  void applicationWillResignActive();
  void savedExplicitly();
  bool ensureWorkingCopyDurableNow();
  State state() const noexcept { return current_state; }
  QString stateText() const { return state_text; }
  int pendingOperationCount() const;
  const QVector<Collaborator> &collaborators() const { return presence; }
  const ManagedMapWorkspace &managedWorkspace() const { return workspace; }
  QDateTime serverTime() const { return server_time; }
  QDateTime estimatedServerTime() const;
  int presenceTtlSeconds() const { return presence_ttl_seconds; }
  void retryNow();
  bool checkpointStreamHead(qint64 *sequence, QString *hash,
                            QString *entity_index_sha256);

signals:
  void stateChanged(OpenOrienteering::MapHubSyncController::State state,
                    const QString &text);
  void upstreamChangeDetected(const QString &message);
  void detailsChanged();

private:
  void clearSession(bool announce_away);
  void observeRevision();
  void scheduleSnapshot(bool urgent = false);
  void stageSnapshot();
  bool stageSnapshotNow(bool resume_after_inbox = true);
  bool finishDurableInbox(bool resume_after_inbox);
  void acknowledgeAppliedOperations();
  void sendFinalAwayHeartbeat();
  void schedulePresenceExpiry();
  void pollSyncState();
  void pullOperations();
  void restoreProjection();
  void queueCommittedEdit(const UndoStep *step);
  void scheduleStructureChanges();
  void queueStructureChanges();
  void drainOutbox();
  void uploadPendingSnapshot();
  void preparePublishedCompaction();
  void pullPublishedCompactionTail(qint64 after_sequence,
                                   const QString &after_hash,
                                   quint64 generation,
                                   const QString &workspace_id);
  void finishPublishedCompaction(quint64 generation,
                                 const QString &workspace_id);
  void failPublishedCompaction(const QString &message, bool retryable);
  void retryWork();
  void setState(State state, const QString &text);
  bool saveWorkspace();
  bool commitWorkingCopy(const QString &snapshot_path, qint64 expected_size,
                         QString *error = nullptr);
  bool commitRequiredWorkingCopy(const QString &snapshot_path,
                                 qint64 expected_size);
  bool retryRequiredWorkingCopy();
  void markCheckpointRequired();
  bool isCurrentSession(quint64 generation,
                        const QString &workspace_id) const;
  bool isCurrentCompactionSession(quint64 generation,
                                  const QString &workspace_id) const;
  bool editable() const;
  bool enqueueOperations(QVector<MapHubEditOperation> operations,
                         QString *error);

  struct LocalEntity {
    QString kind;
    QString parent_id;
    QString after_id;
    QString payload;

    bool operator==(const LocalEntity &other) const {
      return kind == other.kind && parent_id == other.parent_id &&
             after_id == other.after_id && payload == other.payload;
    }
  };
  QHash<QString, LocalEntity> captureLocalEntities() const;
  void refreshObjectCache(const QVector<MapHubEditOperation> &operations);

  ManagedMapWorkspace workspace;
  Map *map = nullptr;
  std::unique_ptr<MapHubOperationStore> operation_store;
  QMetaObject::Connection edit_connection;
  QVector<QMetaObject::Connection> entity_connections;
  RevisionProvider revision_provider;
  SnapshotProvider snapshot_provider;
  WorkingCopyCommitter working_copy_committer;
  QTimer *watch_timer;
  QTimer *snapshot_idle_timer;
  QTimer *snapshot_max_timer;
  QTimer *upload_idle_timer;
  QTimer *retry_timer;
  QTimer *poll_timer;
  QTimer *presence_expiry_timer;
  MapHubApiClient *request_client = nullptr;
  quint64 observed_revision = 0;
  quint64 staged_revision = 0;
  quint64 semantic_revision = 0;
  bool upload_pending = false;
  bool poll_pending = false;
  bool pull_pending = false;
  bool restore_pending = false;
  bool stopped_for_conflict = false;
  bool acknowledgement_pending = false;
  bool acknowledgement_dirty = false;
  bool retry_outbox_now = false;
  bool structure_change_pending = false;
  bool application_active = true;
  bool applying_remote_operations = false;
  bool workspace_metadata_pending = false;
  bool workspace_metadata_desired_stopped = false;
  QString workspace_metadata_desired_problem;
  QString workspace_metadata_error_message;
  quint64 session_generation = 0;
  int active_poll_interval_ms = 30'000;
  int idle_poll_interval_ms = 120'000;
  int presence_ttl_seconds = 0;
  QDateTime server_time;
  QElapsedTimer server_time_age;
  QVector<Collaborator> presence;
  std::unique_ptr<Map> compaction_map;
  quint64 compaction_generation = 0;
  QString compaction_workspace_id;
  qint64 compaction_target_sequence = 0;
  QString compaction_target_hash;
  QString compaction_file_path;
  QHash<QString, LocalEntity> local_entities;
  State current_state = State::Disconnected;
  QString state_text;
};

} // namespace OpenOrienteering

#endif
