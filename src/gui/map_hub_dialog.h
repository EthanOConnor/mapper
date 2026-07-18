/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_DIALOG_H
#define OPENORIENTEERING_MAP_HUB_DIALOG_H

#include <QDialog>
#include <QJsonObject>

#include "collaboration/map_hub_api_client.h"

class QLabel;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace OpenOrienteering {

class MainWindow;
struct ManagedMapWorkspace;

class MapHubDialog final : public QDialog {
  Q_OBJECT
public:
  explicit MapHubDialog(MainWindow *window);
  ~MapHubDialog() override;

private slots:
  void refresh();
  void startSelectedAssignment();
  void openSelectedProject();
  void createConnectedMap();
  void updateActions();

private:
  void showError(const QString &title, const MapHubApiClient::Error &error);
  void populate(const QJsonObject &response);
  void beginWorkspace(const QJsonObject &response, const QString &assignment_id,
                      const QString &project_title,
                      const ManagedMapWorkspace &defaults);
  QString projectTitle(const QString &project_id) const;
  QString uniqueDestination(const QString &project_title,
                            const QString &project_id,
                            const QString &workspace_id, int revision_number,
                            const QString &extension) const;
  void setBusy(bool busy, const QString &message = {});

  MainWindow *window;
  MapHubApiClient *client;
  QLabel *connection_label;
  QLabel *activity_label;
  QTabWidget *tabs;
  QTreeWidget *assignment_list;
  QTreeWidget *project_list;
  QPushButton *start_button;
  QPushButton *open_project_button;
  QPushButton *new_button;
  QPushButton *refresh_button;
  QJsonObject library_response;
  bool busy = false;
};

} // namespace OpenOrienteering

#endif
