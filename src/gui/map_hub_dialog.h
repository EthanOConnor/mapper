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
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

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
  void browseFirstUseWorkspace();
  void connectExistingAccount();
  void redeemFirstUseInvitation();

private:
  void showFirstUse(const QString &problem = {});
  void showLibrary();
  void setFirstUseBusy(bool busy, const QString &message = {});
  bool firstUseConnection(QString &server, QString &workspace_root);
  bool saveFirstUseConnection(const QString &server,
                              const QString &workspace_root,
                              const QString &token, QString &error);
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
  QStackedWidget *pages;
  QWidget *first_use_page;
  QWidget *library_page;
  QLabel *first_use_status;
  QLineEdit *first_use_server;
  QLineEdit *first_use_workspace;
  QLineEdit *first_use_token;
  QLineEdit *first_use_invite;
  QLineEdit *first_use_username;
  QLineEdit *first_use_display_name;
  QLineEdit *first_use_password;
  QPushButton *first_use_browse;
  QPushButton *connect_button;
  QPushButton *redeem_button;
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
