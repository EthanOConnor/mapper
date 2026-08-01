/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_DEVICE_AUTHORIZATION_H
#define OPENORIENTEERING_MAP_HUB_DEVICE_AUTHORIZATION_H

#include <QString>
#include <QUrl>

#include "collaboration/map_hub_api_client.h"

class QTimer;

namespace OpenOrienteering {

/**
 * Drives one browser-mediated Map Hub sign-in without owning any UI.
 *
 * The browser authenticates the person, normally with a passkey. This object
 * retains the short-lived device secret, polls only the exact server origin,
 * and returns the resulting bearer credential to its caller for secure
 * storage. Both first-use and Settings use this single implementation.
 */
class MapHubDeviceAuthorization final : public QObject {
  Q_OBJECT
public:
  struct Result {
    QString token;
    QString organization_name;
  };

  explicit MapHubDeviceAuthorization(QString server_url, QString client_name,
                                     QObject *parent = nullptr);

  void start();
  void cancel();
  bool isRunning() const { return running; }

signals:
  void verificationRequired(const QUrl &url, const QString &user_code);
  void completed(const Result &result, const MapHubApiClient::Error &error);

private:
  void poll();
  void finish(const Result &result, const MapHubApiClient::Error &error);

  MapHubApiClient *client;
  QTimer *poll_timer;
  QString client_name;
  QString request_id;
  QString device_secret;
  bool running = false;
};

} // namespace OpenOrienteering

#endif
