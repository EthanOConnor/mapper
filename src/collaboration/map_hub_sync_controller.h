/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_SYNC_CONTROLLER_H
#define OPENORIENTEERING_MAP_HUB_SYNC_CONTROLLER_H

#include <functional>

#include <QObject>
#include <QString>

#include "collaboration/managed_map_workspace.h"

class QTimer;

namespace OpenOrienteering {

class Map;
class MapHubApiClient;

/**
 * Keeps a durable native OMAP snapshot available to the user's other devices.
 *
 * Complete files are the synchronization unit. If another device advanced the
 * workspace, Map Hub retains both files and this controller stops until the
 * user compares them; it never rebases editor operations invisibly.
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
  bool checkpointFileVersion(QString *version_id);

signals:
  void stateChanged(OpenOrienteering::MapHubSyncController::State state,
                    const QString &text);
  void upstreamChangeDetected(const QString &message);

private:
  void observeRevision();
  void stageAndUpload();
  void uploadStagedFile(const QString &path, const QString &sha256,
                        qint64 size_bytes, quint64 map_revision);
  void pollFileState();
  void setState(State state, const QString &text);
  bool editable() const;
  bool saveWorkspace();

  ManagedMapWorkspace workspace;
  Map *map = nullptr;
  RevisionProvider revision_provider;
  SnapshotProvider snapshot_provider;
  QTimer *watch_timer;
  QTimer *stage_timer;
  QTimer *retry_timer;
  QTimer *poll_timer;
  MapHubApiClient *request_client = nullptr;
  quint64 observed_revision = 0;
  quint64 staged_revision = 0;
  bool upload_pending = false;
  bool poll_pending = false;
  bool stopped_for_conflict = false;
  QString staged_path;
  QString staged_sha256;
  State current_state = State::Disconnected;
  QString state_text;
};

} // namespace OpenOrienteering

#endif
