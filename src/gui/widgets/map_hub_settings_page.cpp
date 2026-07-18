/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_settings_page.h"

#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QToolButton>

#include "collaboration/map_hub_api_client.h"
#include "collaboration/map_hub_credentials.h"
#include "core/document_path.h"
#include "gui/util_gui.h"
#include "imagery/tile_network_manager.h"
#include "settings.h"

namespace OpenOrienteering {

MapHubSettingsPage::MapHubSettingsPage(QWidget *parent)
    : SettingsPage(parent), server_edit(new QLineEdit(this)),
      workspace_root_edit(new QLineEdit(this)), token_edit(new QLineEdit(this)),
      credential_status(new QLabel(this)),
      test_button(new QPushButton(tr("Test connection"), this)),
      clear_button(new QPushButton(tr("Disconnect"), this)),
      invite_edit(new QLineEdit(this)), username_edit(new QLineEdit(this)),
      display_name_edit(new QLineEdit(this)),
      password_edit(new QLineEdit(this)),
      redeem_button(new QPushButton(tr("Create account and connect"), this)) {
  auto *layout = new QFormLayout(this);
  layout->addRow(Util::Headline::create(tr("Map Hub server")));
  server_edit->setPlaceholderText(QStringLiteral("https://maps.example.org"));
  layout->addRow(tr("Server URL:"), server_edit);

  auto *workspace_widget = new QWidget(this);
  auto *workspace_layout = new QHBoxLayout(workspace_widget);
  workspace_layout->setContentsMargins({});
  workspace_layout->addWidget(workspace_root_edit);
  auto *browse = new QToolButton(workspace_widget);
  browse->setText(QStringLiteral("…"));
  workspace_layout->addWidget(browse);
  layout->addRow(tr("Local workspaces:"), workspace_widget);

  layout->addRow(Util::Headline::create(tr("Connected account")));
  token_edit->setEchoMode(QLineEdit::Password);
  token_edit->setPlaceholderText(
      tr("Paste a Mapper API token to replace the stored token"));
  layout->addRow(tr("Account token:"), token_edit);
  credential_status->setWordWrap(true);
  layout->addRow(credential_status);
  auto *account_buttons = new QWidget(this);
  auto *account_layout = new QHBoxLayout(account_buttons);
  account_layout->setContentsMargins({});
  account_layout->addWidget(test_button);
  account_layout->addWidget(clear_button);
  account_layout->addStretch();
  layout->addRow(account_buttons);

  layout->addRow(Util::Headline::create(tr("Use an emailed invitation")));
  invite_edit->setEchoMode(QLineEdit::Password);
  password_edit->setEchoMode(QLineEdit::Password);
  password_edit->setPlaceholderText(tr("At least 12 characters"));
  layout->addRow(tr("Invitation token:"), invite_edit);
  layout->addRow(tr("Username:"), username_edit);
  layout->addRow(tr("Display name:"), display_name_edit);
  layout->addRow(tr("Password:"), password_edit);
  layout->addRow(redeem_button);

  connect(browse, &QToolButton::clicked, this, [this] {
    auto selected = QFileDialog::getExistingDirectory(
        this, tr("Choose Map Hub workspace folder"),
        workspace_root_edit->text());
    if (!selected.isEmpty())
      workspace_root_edit->setText(selected);
  });
  connect(test_button, &QPushButton::clicked, this,
          &MapHubSettingsPage::testConnection);
  connect(clear_button, &QPushButton::clicked, this,
          &MapHubSettingsPage::clearCredential);
  connect(redeem_button, &QPushButton::clicked, this,
          &MapHubSettingsPage::redeemInvite);
  reset();
}

QString MapHubSettingsPage::title() const { return tr("Map Hub"); }

QString MapHubSettingsPage::effectiveToken() const {
  if (!token_edit->text().trimmed().isEmpty())
    return token_edit->text().trimmed();
  return MapHubCredentials::readToken(server_edit->text().trimmed()).token;
}

void MapHubSettingsPage::updateCredentialStatus() {
  auto result = MapHubCredentials::readToken(server_edit->text().trimmed());
  if (!result.error.isEmpty())
    credential_status->setText(tr("Credential error: %1").arg(result.error));
  else if (result.token.isEmpty())
    credential_status->setText(
        tr("Not connected. You can paste a token or redeem an invitation."));
  else if (result.used_fallback)
    credential_status->setText(
        tr("Connected. This system has no desktop secret service, so the token "
           "is in an owner-only application file."));
  else
    credential_status->setText(tr("Connected. The account token is stored in "
                                  "the operating system credential store."));
  clear_button->setEnabled(!result.token.isEmpty());
}

void MapHubSettingsPage::apply() {
  auto server = server_edit->text().trimmed();
  auto url = QUrl::fromUserInput(server);
  if (!MapHubApiClient::isAcceptableServerUrl(url)) {
    emit applyFailed();
    QMessageBox::warning(this, tr("Map Hub"),
                         tr("Enter an HTTPS Map Hub URL. HTTP is allowed only "
                            "for localhost development."));
    return;
  }
  auto workspace_root = workspace_root_edit->text().trimmed();
  if (DocumentPath::isContentUri(workspace_root)) {
    emit applyFailed();
    QMessageBox::warning(
        this, tr("Map Hub"),
        tr("Map Hub needs a local workspace directory so it can preserve "
           "original baselines beside normalized .omap files. Choose a local "
           "folder; individual maps can still be saved or exported through "
           "the document provider."));
    return;
  }
  if (!token_edit->text().trimmed().isEmpty()) {
    auto result =
        MapHubCredentials::writeToken(server, token_edit->text().trimmed());
    if (!result) {
      emit applyFailed();
      QMessageBox::warning(this, tr("Map Hub"), result.error);
      return;
    }
    token_edit->clear();
  }
  if (loaded_server != server)
    imagery::TileNetworkManager::instance().clearBearerCredential(
        QUrl(loaded_server));
  auto stored = MapHubCredentials::readToken(server);
  if (!stored.token.isEmpty())
    imagery::TileNetworkManager::instance().setBearerCredential(
        QUrl(server), stored.token.toUtf8(),
        MapHubCredentials::accountName(server).toUtf8());
  setSetting(Settings::MapHub_ServerUrl, server);
  setSetting(Settings::MapHub_WorkspaceRoot, workspace_root);
  loaded_server = server;
  updateCredentialStatus();
}

void MapHubSettingsPage::reset() {
  loaded_server = getSetting(Settings::MapHub_ServerUrl).toString();
  server_edit->setText(loaded_server);
  auto root = getSetting(Settings::MapHub_WorkspaceRoot).toString();
  if (root.isEmpty()) {
#ifdef Q_OS_ANDROID
    root =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("map-hub-workspaces"));
#else
    root = QDir(QStandardPaths::writableLocation(
                    QStandardPaths::DocumentsLocation))
               .filePath(tr("Mapper Workspaces"));
#endif
  }
  workspace_root_edit->setText(root);
  token_edit->clear();
  invite_edit->clear();
  password_edit->clear();
  updateCredentialStatus();
}

void MapHubSettingsPage::setBusy(bool busy) {
  test_button->setEnabled(!busy);
  redeem_button->setEnabled(!busy);
  server_edit->setEnabled(!busy);
}

void MapHubSettingsPage::testConnection() {
  MapHubApiClient client(server_edit->text().trimmed(), effectiveToken(), this);
  if (!client.isConfigured()) {
    QMessageBox::warning(this, tr("Map Hub"), client.configurationError());
    return;
  }
  setBusy(true);
  auto *request_client = new MapHubApiClient(server_edit->text().trimmed(),
                                             effectiveToken(), this);
  request_client->library(
      [this, request_client](const QJsonObject &response,
                             const MapHubApiClient::Error &error) {
        setBusy(false);
        request_client->deleteLater();
        if (error)
          QMessageBox::warning(this, tr("Map Hub connection failed"),
                               error.message);
        else
          QMessageBox::information(
              this, tr("Map Hub"),
              tr("Connected to %1.")
                  .arg(response.value(QStringLiteral("organization"))
                           .toObject()
                           .value(QStringLiteral("name"))
                           .toString()));
      });
}

void MapHubSettingsPage::clearCredential() {
  imagery::TileNetworkManager::instance().clearBearerCredential(
      QUrl(server_edit->text().trimmed()));
  auto result = MapHubCredentials::removeToken(server_edit->text().trimmed());
  if (!result)
    QMessageBox::warning(this, tr("Map Hub"), result.error);
  token_edit->clear();
  updateCredentialStatus();
}

void MapHubSettingsPage::redeemInvite() {
  QJsonObject account{
      {QStringLiteral("invite_token"), invite_edit->text().trimmed()},
      {QStringLiteral("username"), username_edit->text().trimmed()},
      {QStringLiteral("display_name"), display_name_edit->text().trimmed()},
      {QStringLiteral("password"), password_edit->text()},
  };
  if (account.value(QStringLiteral("invite_token")).toString().isEmpty() ||
      account.value(QStringLiteral("username")).toString().isEmpty() ||
      account.value(QStringLiteral("display_name")).toString().isEmpty() ||
      password_edit->text().size() < 12) {
    QMessageBox::warning(this, tr("Map Hub"),
                         tr("Enter the invitation, username, display name, and "
                            "a password of at least 12 characters."));
    return;
  }
  setBusy(true);
  auto *client = new MapHubApiClient(server_edit->text().trimmed(), {}, this);
  client->redeemInvite(account, [this,
                                 client](const QJsonObject &response,
                                         const MapHubApiClient::Error &error) {
    setBusy(false);
    client->deleteLater();
    password_edit->clear();
    if (error) {
      QMessageBox::warning(this, tr("Could not create account"), error.message);
      return;
    }
    auto result = MapHubCredentials::writeToken(
        server_edit->text().trimmed(),
        response.value(QStringLiteral("token")).toString());
    if (!result) {
      QMessageBox::warning(this, tr("Account created, but not connected"),
                           result.error);
      return;
    }
    imagery::TileNetworkManager::instance().setBearerCredential(
        QUrl(server_edit->text().trimmed()),
        response.value(QStringLiteral("token")).toString().toUtf8(),
        MapHubCredentials::accountName(server_edit->text().trimmed()).toUtf8());
    invite_edit->clear();
    setSetting(Settings::MapHub_ServerUrl, server_edit->text().trimmed());
    updateCredentialStatus();
    QMessageBox::information(
        this, tr("Map Hub"),
        tr("Your account is connected. Mapper can now open your assignments."));
  });
}

} // namespace OpenOrienteering
