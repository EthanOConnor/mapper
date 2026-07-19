/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_SETTINGS_PAGE_H
#define OPENORIENTEERING_MAP_HUB_SETTINGS_PAGE_H

#include "gui/widgets/settings_page.h"

class QLabel;
class QLineEdit;
class QPushButton;

namespace OpenOrienteering {

class MapHubSettingsPage final : public SettingsPage {
  Q_OBJECT
public:
  explicit MapHubSettingsPage(QWidget *parent = nullptr);
  QString title() const override;

public slots:
  void apply() override;
  void reset() override;

private slots:
  void testConnection();
  void clearCredential();
  void openInvitation();

private:
  QString effectiveToken() const;
  void updateCredentialStatus();
  void setBusy(bool busy);

  QLineEdit *server_edit;
  QLineEdit *workspace_root_edit;
  QLineEdit *token_edit;
  QLabel *credential_status;
  QPushButton *test_button;
  QPushButton *clear_button;
  QLineEdit *invite_edit;
  QPushButton *invitation_button;
  QString loaded_server;
};

} // namespace OpenOrienteering

#endif
