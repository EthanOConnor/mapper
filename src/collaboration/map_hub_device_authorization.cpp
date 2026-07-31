/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_device_authorization.h"

#include <QTimer>
#include <QUuid>

namespace OpenOrienteering {

namespace {

constexpr auto min_poll_seconds = 1;
constexpr auto max_poll_seconds = 10;

bool sameOrigin(const QUrl &first, const QUrl &second) {
  return first.scheme().compare(second.scheme(), Qt::CaseInsensitive) == 0 &&
         first.host().compare(second.host(), Qt::CaseInsensitive) == 0 &&
         first.port(first.scheme() == QLatin1String("https") ? 443 : 80) ==
             second.port(second.scheme() == QLatin1String("https") ? 443 : 80);
}

MapHubApiClient::Error invalidResponse(QString message) {
  return {0, QStringLiteral("invalid_response"), std::move(message)};
}

} // namespace

MapHubDeviceAuthorization::MapHubDeviceAuthorization(QString server_url,
                                                     QString client_name,
                                                     QObject *parent)
    : QObject(parent),
      client(new MapHubApiClient(std::move(server_url), {}, this)),
      poll_timer(new QTimer(this)), client_name(std::move(client_name)) {
  poll_timer->setSingleShot(false);
  connect(poll_timer, &QTimer::timeout, this,
          &MapHubDeviceAuthorization::poll);
}

void MapHubDeviceAuthorization::start() {
  if (running)
    return;
  running = true;
  client->startMapperConnection(
      client_name,
      [this](const QJsonObject &response, const MapHubApiClient::Error &error) {
        if (error) {
          finish({}, error);
          return;
        }
        request_id = response.value(QStringLiteral("request_id")).toString();
        device_secret =
            response.value(QStringLiteral("device_secret")).toString();
        auto verification_url =
            QUrl(response.value(QStringLiteral("verification_url")).toString());
        auto interval = response.value(QStringLiteral("interval")).toInt();
        if (QUuid(request_id).isNull() || device_secret.isEmpty() ||
            device_secret.toUtf8().size() > 200 || !verification_url.isValid() ||
            verification_url.userInfo().size() ||
            !sameOrigin(client->serverUrl(), verification_url) || interval < 1) {
          finish({}, invalidResponse(tr("Map Hub returned an invalid sign-in response.")));
          return;
        }
        interval = qBound(min_poll_seconds, interval, max_poll_seconds);
        emit verificationRequired(verification_url,
                                  response.value(QStringLiteral("user_code"))
                                      .toString());
        poll_timer->start(interval * 1000);
        QTimer::singleShot(
            qBound(10, response.value(QStringLiteral("expires_in")).toInt(),
                   600) * 1000,
            this, [this] {
              if (running)
                finish({}, {0, QStringLiteral("connection_expired"),
                            tr("Map Hub sign-in expired before it was approved.")});
            });
      });
}

void MapHubDeviceAuthorization::cancel() {
  if (running)
    finish({}, {0, QStringLiteral("cancelled"), tr("Map Hub sign-in was cancelled.")});
}

void MapHubDeviceAuthorization::poll() {
  if (!running)
    return;
  client->exchangeMapperConnection(
      request_id, device_secret,
      [this](const QJsonObject &response, const MapHubApiClient::Error &error) {
        if (error) {
          finish({}, error);
          return;
        }
        const auto status = response.value(QStringLiteral("status")).toString();
        if (status == QLatin1String("pending"))
          return;
        if (status != QLatin1String("connected")) {
          finish({}, invalidResponse(tr("Map Hub returned an invalid sign-in status.")));
          return;
        }
        Result result;
        result.token = response.value(QStringLiteral("token")).toString();
        result.organization_name =
            response.value(QStringLiteral("organization"))
                .toObject()
                .value(QStringLiteral("name"))
                .toString();
        if (result.token.isEmpty() || result.token.toUtf8().size() > 4096) {
          finish({}, invalidResponse(tr("Map Hub returned an invalid account credential.")));
          return;
        }
        finish(result, {});
      });
}

void MapHubDeviceAuthorization::finish(const Result &result,
                                       const MapHubApiClient::Error &error) {
  if (!running)
    return;
  running = false;
  poll_timer->stop();
  request_id.clear();
  device_secret.clear();
  emit completed(result, error);
}

} // namespace OpenOrienteering
