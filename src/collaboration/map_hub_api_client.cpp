/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_api_client.h"

#include <memory>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

namespace OpenOrienteering {

namespace {

constexpr qint64 max_json_response_bytes = 8LL * 1024 * 1024;
constexpr qint64 max_artifact_bytes = 2LL * 1024 * 1024 * 1024;

int effectivePort(const QUrl &url) {
  return url.port(url.scheme() == QLatin1String("https") ? 443 : 80);
}

QHttpPart textPart(const QByteArray &name, const QString &value) {
  QHttpPart part;
  part.setHeader(
      QNetworkRequest::ContentDispositionHeader,
      QStringLiteral("form-data; name=\"%1\"").arg(QString::fromLatin1(name)));
  part.setBody(value.toUtf8());
  return part;
}

bool validStableId(const QString &id) { return !QUuid(id).isNull(); }

bool validHeaderValue(const QString &value, int maximum) {
  auto bytes = value.toUtf8();
  return !bytes.isEmpty() && bytes.size() <= maximum && !bytes.contains('\r') &&
         !bytes.contains('\n') && !bytes.contains('\0');
}

MapHubApiClient::Error invalidIdentifierError() {
  return {0, QStringLiteral("invalid_identifier"),
          MapHubApiClient::tr(
              "Map Hub returned an invalid stable identifier; no request was "
              "sent.")};
}

struct ArtifactDownloadState {
  QCryptographicHash hash{QCryptographicHash::Sha256};
  qint64 received = 0;
  MapHubApiClient::Error error;
};

void consumeArtifactData(QNetworkReply *reply, QSaveFile *file,
                         ArtifactDownloadState *state, bool may_abort) {
  if (state->error)
    return;
  const auto data = reply->readAll();
  if (data.size() > max_artifact_bytes - state->received) {
    state->error = {
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        QStringLiteral("response_too_large"),
        MapHubApiClient::tr(
            "The map artifact exceeds Mapper's 2 GiB download limit.")};
  } else {
    state->received += data.size();
    state->hash.addData(data);
    if (file->write(data) != data.size()) {
      auto message = file->errorString();
      if (message.isEmpty())
        message = MapHubApiClient::tr(
            "Mapper could not write the complete downloaded map.");
      state->error = {0, QStringLiteral("local_file"), message};
    }
  }
  if (state->error && may_abort)
    reply->abort();
}

} // namespace

MapHubApiClient::MapHubApiClient(QString server_url, QString bearer_token,
                                 QObject *parent)
    : QObject(parent),
      server_url(
          QUrl::fromUserInput(server_url).adjusted(QUrl::StripTrailingSlash)),
      bearer_token(std::move(bearer_token)),
      network(new QNetworkAccessManager(this)) {}

bool MapHubApiClient::isAcceptableServerUrl(const QUrl &url) {
  if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty())
    return false;
  if ((!url.path().isEmpty() && url.path() != QLatin1String("/")) ||
      !url.query().isEmpty() || !url.fragment().isEmpty())
    return false;
  if (url.scheme() == QLatin1String("https"))
    return true;
  return url.scheme() == QLatin1String("http") &&
         (url.host() == QLatin1String("localhost") ||
          url.host() == QLatin1String("127.0.0.1") ||
          url.host() == QLatin1String("::1"));
}

bool MapHubApiClient::isMapperWorkspacePackageType(
    const QString &package_type) {
  return package_type == QLatin1String("basemap") ||
         package_type == QLatin1String("new_mapping") ||
         package_type == QLatin1String("remap") ||
         package_type == QLatin1String("update") ||
         package_type == QLatin1String("field_check") ||
         package_type == QLatin1String("review");
}

bool MapHubApiClient::isConfigured() const {
  return isAcceptableServerUrl(server_url) && !bearer_token.trimmed().isEmpty();
}

QString MapHubApiClient::configurationError() const {
  if (!isAcceptableServerUrl(server_url))
    return tr("Map Hub requires an HTTPS server URL (HTTP is allowed only for "
              "localhost development).");
  if (bearer_token.trimmed().isEmpty())
    return tr("No Map Hub account token is stored for this server.");
  return {};
}

bool MapHubApiClient::isSameOrigin(const QUrl &url) const {
  return url.scheme().compare(server_url.scheme(), Qt::CaseInsensitive) == 0 &&
         url.host().compare(server_url.host(), Qt::CaseInsensitive) == 0 &&
         effectivePort(url) == effectivePort(server_url) &&
         url.userInfo().isEmpty();
}

QNetworkRequest MapHubApiClient::request(const QString &relative_path,
                                         bool authenticated) const {
  auto base = server_url;
  auto path = relative_path;
  if (!path.startsWith(QLatin1Char('/')))
    path.prepend(QLatin1Char('/'));
  base.setPath(path);
  base.setQuery({});
  base.setFragment({});
  return request(base, authenticated);
}

QNetworkRequest MapHubApiClient::request(const QUrl &url,
                                         bool authenticated) const {
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                   QNetworkRequest::SameOriginRedirectPolicy);
  req.setTransferTimeout(120000);
  req.setRawHeader("Accept", "application/json");
  req.setRawHeader("User-Agent", "OpenOrienteering-Mapper/MapHub-v1");
  if (authenticated && isSameOrigin(url))
    req.setRawHeader("Authorization",
                     QByteArray("Bearer ") + bearer_token.toUtf8());
  return req;
}

MapHubApiClient::Error MapHubApiClient::replyError(QNetworkReply *reply,
                                                   const QByteArray &body) {
  Error error;
  error.http_status =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  QJsonParseError parse_error;
  auto document = QJsonDocument::fromJson(body, &parse_error);
  if (parse_error.error == QJsonParseError::NoError) {
    auto object = document.object().value(QStringLiteral("error")).toObject();
    error.code = object.value(QStringLiteral("code")).toString();
    error.message = object.value(QStringLiteral("message")).toString();
  }
  if (error.message.isEmpty())
    error.message = reply->errorString();
  if (error.code.isEmpty())
    error.code = reply->error() == QNetworkReply::OperationCanceledError
                     ? QStringLiteral("cancelled")
                     : QStringLiteral("network_error");
  return error;
}

void MapHubApiClient::finishJson(QNetworkReply *reply, JsonHandler handler) {
  auto body = std::make_shared<QByteArray>();
  auto too_large = std::make_shared<bool>(false);
  reply->setReadBufferSize(max_json_response_bytes + 1);
  connect(reply, &QIODevice::readyRead, this, [reply, body, too_large] {
    if (*too_large)
      return;
    body->append(reply->readAll());
    if (body->size() > max_json_response_bytes) {
      *too_large = true;
      reply->abort();
    }
  });
  connect(
      reply, &QNetworkReply::finished, this,
      [reply, body, too_large, handler = std::move(handler)]() mutable {
        if (!*too_large) {
          body->append(reply->readAll());
          *too_large = body->size() > max_json_response_bytes;
        }
        if (*too_large) {
          handler({},
                  {reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                       .toInt(),
                   QStringLiteral("response_too_large"),
                   MapHubApiClient::tr(
                       "Map Hub returned more than 8 MiB of JSON; Mapper "
                       "discarded the response.")});
          reply->deleteLater();
          return;
        }
        if (reply->error() != QNetworkReply::NoError) {
          handler({}, replyError(reply, *body));
          reply->deleteLater();
          return;
        }
        QJsonParseError parse_error;
        auto document = QJsonDocument::fromJson(*body, &parse_error);
        if (parse_error.error != QJsonParseError::NoError ||
            !document.isObject()) {
          handler({},
                  {reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                       .toInt(),
                   QStringLiteral("invalid_response"),
                   MapHubApiClient::tr(
                       "Map Hub returned an invalid JSON response.")});
        } else {
          handler(document.object(), {});
        }
        reply->deleteLater();
      });
}

void MapHubApiClient::sendJson(const QByteArray &method,
                               const QString &relative_path,
                               const QJsonObject &body, bool authenticated,
                               JsonHandler handler) {
  if (!ensureReady(authenticated, handler))
    return;
  auto req = request(relative_path, authenticated);
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json"));
  auto data = QJsonDocument(body).toJson(QJsonDocument::Compact);
  auto *reply = network->sendCustomRequest(req, method, data);
  finishJson(reply, std::move(handler));
}

bool MapHubApiClient::ensureReady(bool authenticated,
                                  const JsonHandler &handler) const {
  if (!isAcceptableServerUrl(server_url)) {
    handler({}, {0, QStringLiteral("invalid_configuration"),
                 tr("Map Hub requires an HTTPS server URL (HTTP is allowed "
                    "only for localhost development).")});
    return false;
  }
  if (authenticated && bearer_token.trimmed().isEmpty()) {
    handler({}, {0, QStringLiteral("invalid_configuration"),
                 tr("No Map Hub account token is stored for this server.")});
    return false;
  }
  return true;
}

void MapHubApiClient::health(JsonHandler handler) {
  if (!ensureReady(false, handler))
    return;
  finishJson(network->get(request(QStringLiteral("/api/v1/health"), false)),
             std::move(handler));
}

void MapHubApiClient::library(JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  finishJson(network->get(request(QStringLiteral("/api/v1/library"))),
             std::move(handler));
}

void MapHubApiClient::projectManifest(const QString &project_id,
                                      JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!validStableId(project_id)) {
    handler({}, invalidIdentifierError());
    return;
  }
  finishJson(
      network->get(request(
          QStringLiteral("/api/v1/projects/%1/manifest").arg(project_id))),
      std::move(handler));
}

void MapHubApiClient::createProject(const QJsonObject &project,
                                    const QString &idempotency_key,
                                    JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!validHeaderValue(idempotency_key, 120)) {
    handler({}, {0, QStringLiteral("invalid_idempotency_key"),
                 tr("The project creation transaction key is invalid.")});
    return;
  }
  auto req = request(QStringLiteral("/api/v1/projects"));
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json"));
  req.setRawHeader("Idempotency-Key", idempotency_key.toUtf8());
  auto *reply =
      network->post(req, QJsonDocument(project).toJson(QJsonDocument::Compact));
  finishJson(reply, std::move(handler));
}

void MapHubApiClient::startAssignment(const QString &assignment_id,
                                      JsonHandler handler) {
  if (!validStableId(assignment_id)) {
    handler({}, invalidIdentifierError());
    return;
  }
  sendJson("POST",
           QStringLiteral("/api/v1/assignments/%1/start").arg(assignment_id),
           {}, true, std::move(handler));
}

void MapHubApiClient::checkpoint(
    const QString &workspace_id, const QString &file_path,
    const QString &base_revision_id, const QString &editing_lease,
    const QString &label, const QString &change_summary,
    const QString &idempotency_key, JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!validStableId(workspace_id)) {
    handler({}, invalidIdentifierError());
    return;
  }
  if ((!base_revision_id.isEmpty() && !validStableId(base_revision_id)) ||
      !validHeaderValue(idempotency_key, 120) ||
      (!editing_lease.isEmpty() && !validHeaderValue(editing_lease, 4096))) {
    handler({}, {0, QStringLiteral("invalid_request_metadata"),
                 tr("The checkpoint transaction or editing lease is "
                    "invalid.")});
    return;
  }
  auto req = request(
      QStringLiteral("/api/v1/workspaces/%1/checkpoint").arg(workspace_id));
  req.setRawHeader("Idempotency-Key", idempotency_key.toUtf8());
  if (!editing_lease.isEmpty())
    req.setRawHeader("X-Editing-Lease", editing_lease.toUtf8());
  auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
  multi->append(textPart("base_revision_id", base_revision_id));
  multi->append(textPart("label", label));
  multi->append(textPart("change_summary", change_summary));
  QHttpPart file_part;
  file_part.setHeader(
      QNetworkRequest::ContentDispositionHeader,
      QStringLiteral("form-data; name=\"file\"; filename=\"workspace.omap\""));
  file_part.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/vnd.openorienteering.omap"));
  auto *file = new QFile(file_path, multi);
  if (!file->open(QIODevice::ReadOnly)) {
    auto message = file->errorString();
    delete multi;
    handler({}, {0, QStringLiteral("local_file"), message});
    return;
  }
  if (file->size() > max_artifact_bytes) {
    delete multi;
    handler({}, {0, QStringLiteral("local_file"),
                 tr("The map workspace exceeds the 2 GiB upload limit.")});
    return;
  }
  file_part.setBodyDevice(file);
  multi->append(file_part);
  auto *reply = network->post(req, multi);
  multi->setParent(reply);
  finishJson(reply, std::move(handler));
}

void MapHubApiClient::submitRevision(const QString &revision_id,
                                     const QString &editing_lease,
                                     JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!validStableId(revision_id) ||
      (!editing_lease.isEmpty() && !validHeaderValue(editing_lease, 4096))) {
    handler({}, invalidIdentifierError());
    return;
  }
  auto req =
      request(QStringLiteral("/api/v1/revisions/%1/submit").arg(revision_id));
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json"));
  if (!editing_lease.isEmpty())
    req.setRawHeader("X-Editing-Lease", editing_lease.toUtf8());
  finishJson(network->post(req, QByteArrayLiteral("{}")), std::move(handler));
}

void MapHubApiClient::renewLease(const QString &workspace_id,
                                 const QString &editing_lease,
                                 JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!validStableId(workspace_id) || !validHeaderValue(editing_lease, 4096)) {
    handler({}, invalidIdentifierError());
    return;
  }
  auto req =
      request(QStringLiteral("/api/v1/workspaces/%1/renew").arg(workspace_id));
  if (!editing_lease.isEmpty())
    req.setRawHeader("X-Editing-Lease", editing_lease.toUtf8());
  finishJson(network->post(req, QByteArray{}), std::move(handler));
}

void MapHubApiClient::redeemInvite(const QJsonObject &account,
                                   JsonHandler handler) {
  sendJson("POST", QStringLiteral("/api/v1/invites/redeem"), account, false,
           std::move(handler));
}

QString MapHubApiClient::sha256ForFile(const QString &path, QString *error) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (error)
      *error = file.errorString();
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!hash.addData(&file)) {
    if (error)
      *error = file.errorString();
    return {};
  }
  return QString::fromLatin1(hash.result().toHex());
}

void MapHubApiClient::downloadArtifact(const QUrl &url,
                                       const QString &expected_sha256,
                                       const QString &destination,
                                       DownloadHandler handler) {
  if (!isAcceptableServerUrl(server_url) || bearer_token.trimmed().isEmpty()) {
    handler({},
            {0, QStringLiteral("invalid_configuration"), configurationError()});
    return;
  }
  static const QRegularExpression sha256_pattern(
      QStringLiteral("^[0-9a-fA-F]{64}$"));
  if (!sha256_pattern.match(expected_sha256).hasMatch()) {
    handler({}, {0, QStringLiteral("invalid_checksum"),
                 tr("Map Hub did not provide a valid SHA-256 checksum for "
                    "this artifact; it was not downloaded.")});
    return;
  }
  if (!isSameOrigin(url)) {
    handler({}, {0, QStringLiteral("untrusted_download"),
                 tr("Map Hub returned an artifact URL on a different origin; "
                    "the token was not sent.")});
    return;
  }
  auto *reply = network->get(request(url));
  auto *file = new QSaveFile(destination, reply);
  if (!file->open(QIODevice::WriteOnly)) {
    auto message = file->errorString();
    reply->abort();
    reply->deleteLater();
    handler({}, {0, QStringLiteral("local_file"), message});
    return;
  }
  auto *state = new ArtifactDownloadState;
  connect(reply, &QNetworkReply::downloadProgress, this,
          &MapHubApiClient::downloadProgress);
  connect(reply, &QNetworkReply::metaDataChanged, this, [reply, state] {
    bool valid = false;
    const auto length =
        reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&valid);
    if (valid && length > max_artifact_bytes && !state->error) {
      state->error = {
          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
          QStringLiteral("response_too_large"),
          MapHubApiClient::tr(
              "The map artifact exceeds Mapper's 2 GiB download "
              "limit.")};
      reply->abort();
    }
  });
  connect(reply, &QIODevice::readyRead, this, [reply, file, state] {
    consumeArtifactData(reply, file, state, true);
  });
  connect(
      reply, &QNetworkReply::finished, this,
      [reply, file, state, destination, expected_sha256,
       handler = std::move(handler)]() mutable {
        consumeArtifactData(reply, file, state, false);
        const auto actual = QString::fromLatin1(state->hash.result().toHex());
        const auto state_error = state->error;
        delete state;
        if (state_error) {
          file->cancelWriting();
          handler({}, state_error);
        } else if (reply->error() != QNetworkReply::NoError) {
          file->cancelWriting();
          handler({}, replyError(reply, {}));
        } else if (actual.compare(expected_sha256, Qt::CaseInsensitive) != 0) {
          file->cancelWriting();
          handler({},
                  {reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                       .toInt(),
                   QStringLiteral("checksum_mismatch"),
                   MapHubApiClient::tr("The downloaded map did not match the "
                                       "server checksum; it was not opened.")});
        } else if (!file->commit()) {
          handler({}, {0, QStringLiteral("local_file"), file->errorString()});
        } else {
          handler(destination, {});
        }
        reply->deleteLater();
      });
}

} // namespace OpenOrienteering
