/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_SYNC_CONTROLLER_H
#define OPENORIENTEERING_MAP_HUB_SYNC_CONTROLLER_H

#include <functional>
#include <memory>

#include <QMetaObject>
#include <QObject>
#include <QString>

#include "collaboration/managed_map_workspace.h"

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
    UpstreamChanged,
    ActionRequired,
  };
  Q_ENUM(State)

  using RevisionProvider = std::function<quint64()>;
  using SnapshotProvider = std::function<bool(
      const QString &destination, quint64 *staged_revision, QString *error)>;

  explicit MapHubSyncController(QObject *parent = nullptr);
  ~MapHubSyncController() override;

  void configure(const ManagedMapWorkspace &workspace, Map *map,
                 RevisionProvider revision_provider,
                 SnapshotProvider snapshot_provider);
  void clear();
  void applicationBecameActive();
  void applicationWillResignActive();
  void savedExplicitly();
  State state() const noexcept { return current_state; }
  QString stateText() const { return state_text; }
  bool checkpointStreamHead(qint64 *sequence, QString *hash,
                            QString *entity_index_sha256) const;

signals:
  void stateChanged(OpenOrienteering::MapHubSyncController::State state,
                    const QString &text);
  void upstreamChangeDetected(const QString &message);

private:
  void observeRevision();
  void scheduleSnapshot(bool urgent = false);
  void stageSnapshot();
  void pollSyncState();
  void pullOperations();
  void restoreProjection();
  void queueCommittedEdit(const UndoStep *step);
  void drainOutbox();
  void uploadPendingSnapshot();
  void retryWork();
  void setState(State state, const QString &text);
  bool saveWorkspace();
  bool editable() const;

  ManagedMapWorkspace workspace;
  Map *map = nullptr;
  std::unique_ptr<MapHubOperationStore> operation_store;
  QMetaObject::Connection edit_connection;
  RevisionProvider revision_provider;
  SnapshotProvider snapshot_provider;
  QTimer *watch_timer;
  QTimer *snapshot_idle_timer;
  QTimer *snapshot_max_timer;
  QTimer *upload_idle_timer;
  QTimer *retry_timer;
  QTimer *poll_timer;
  MapHubApiClient *request_client = nullptr;
  quint64 observed_revision = 0;
  quint64 staged_revision = 0;
  bool upload_pending = false;
  bool poll_pending = false;
  bool pull_pending = false;
  bool restore_pending = false;
  bool stopped_for_conflict = false;
  bool applying_remote_operations = false;
  State current_state = State::Disconnected;
  QString state_text;
};

} // namespace OpenOrienteering

#endif
