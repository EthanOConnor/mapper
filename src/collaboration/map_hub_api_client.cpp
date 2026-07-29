/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_api_client.h"

#include <memory>

#include <QBuffer>
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
#include <QUrlQuery>
#include <QUuid>

#include "collaboration/zstd_codec.h"

namespace OpenOrienteering {

namespace {

constexpr qint64 max_json_response_bytes = 16LL * 1024 * 1024;
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

bool decodeJsonResponse(QNetworkReply *reply, QByteArray *body,
                        MapHubApiClient::Error *error) {
  if (reply->rawHeader("Content-Encoding")
          .compare("zstd", Qt::CaseInsensitive) != 0)
    return true;

  QString decompression_error;
  auto decoded =
      ZstdCodec::decompress(*body, max_json_response_bytes,
                            &decompression_error);
  if (decoded.isEmpty() && !body->isEmpty()) {
    *error = {
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
        QStringLiteral("invalid_compressed_response"), decompression_error};
    return false;
  }
  *body = std::move(decoded);
  return true;
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

MapHubApiClient::WorkspaceBaseline
MapHubApiClient::classifyWorkspaceBaseline(const QJsonObject &revision) {
  if (revision.isEmpty())
    return WorkspaceBaseline::NoRevision;
  const QUrl download_url(
      revision.value(QStringLiteral("download_url")).toString());
  if (!download_url.isValid() || download_url.isEmpty())
    return WorkspaceBaseline::IncompleteRevision;
  return WorkspaceBaseline::ArtifactReference;
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
  base.setQuery(QString{});
  base.setFragment(QString{});
  return request(base, authenticated);
}

QNetworkRequest MapHubApiClient::request(const QUrl &url,
                                         bool authenticated) const {
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                   QNetworkRequest::SameOriginRedirectPolicy);
  req.setTransferTimeout(120000);
  req.setRawHeader("Accept", "application/json");
  req.setRawHeader("Accept-Encoding", "zstd");
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
                       "Map Hub returned more than 16 MiB of JSON; Mapper "
                       "discarded the response.")});
          reply->deleteLater();
          return;
        }
        MapHubApiClient::Error decoding_error;
        if (!decodeJsonResponse(reply, body.get(), &decoding_error)) {
          handler({}, decoding_error);
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

void MapHubApiClient::workspaceOperations(const QString &workspace_id,
                                          qint64 after_sequence, int limit,
                                          JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!validStableId(workspace_id) || after_sequence < 0 || limit < 1 ||
      limit > 256) {
    handler({}, invalidIdentifierError());
    return;
  }
  auto req = request(
      QStringLiteral("/api/v1/workspaces/%1/operations").arg(workspace_id));
  auto url = req.url();
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("after"), QString::number(after_sequence));
  query.addQueryItem(QStringLiteral("limit"), QString::number(limit));
  url.setQuery(query);
  req.setUrl(url);
  finishJson(network->get(req), std::move(handler));
}

void MapHubApiClient::postWorkspaceTransaction(const QString &workspace_id,
                                               const QByteArray &canonical_json,
                                               const QString &editing_lease,
                                               JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!validStableId(workspace_id) || !validHeaderValue(editing_lease, 512)) {
    handler({}, invalidIdentifierError());
    return;
  }
  if (canonical_json.isEmpty() || canonical_json.size() > 1024 * 1024) {
    handler({},
            {0, QStringLiteral("invalid_payload"),
             tr("The Map Hub edit transaction is empty or exceeds 1 MiB.")});
    return;
  }
  auto req = request(
      QStringLiteral("/api/v1/workspaces/%1/transactions").arg(workspace_id));
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json"));
  req.setRawHeader("X-Editing-Lease", editing_lease.toUtf8());
  QByteArray payload = canonical_json;
  if (canonical_json.size() >= 512) {
    auto compressed = ZstdCodec::compress(canonical_json);
    if (!compressed.isEmpty() &&
        compressed.size() + 32 < canonical_json.size()) {
      payload = std::move(compressed);
      req.setRawHeader("Content-Encoding", "zstd");
    }
  }
  finishJson(network->post(req, payload), std::move(handler));
}

void MapHubApiClient::acknowledgeWorkspaceOperations(
    const QString &workspace_id, const QJsonObject &acknowledgement,
    const QString &editing_lease, JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!validStableId(workspace_id) || !validHeaderValue(editing_lease, 512)) {
    handler({}, invalidIdentifierError());
    return;
  }
  auto req =
      request(QStringLiteral("/api/v1/workspaces/%1/ack").arg(workspace_id));
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json"));
  req.setRawHeader("X-Editing-Lease", editing_lease.toUtf8());
  auto *reply = network->post(
      req, QJsonDocument(acknowledgement).toJson(QJsonDocument::Compact));
  finishJson(reply, std::move(handler));
}

void MapHubApiClient::uploadWorkspaceSnapshot(
    const QString &workspace_id, const QString &file_path,
    const QByteArray &canonical_entity_index, qint64 base_stream_sequence,
    const QString &base_stream_hash, const QString &file_sha256,
    qint64 file_size, const QString &workspace_revision_id,
    const QString &project_revision_id, const QString &client_instance_id,
    const QString &editing_lease, const QString &idempotency_key,
    JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  static const QRegularExpression hash_pattern(
      QStringLiteral("^[0-9a-f]{64}$"));
  if (!validStableId(workspace_id) || !validStableId(client_instance_id) ||
      base_stream_sequence < 0 ||
      !hash_pattern.match(base_stream_hash).hasMatch() ||
      !hash_pattern.match(file_sha256).hasMatch() || file_size < 1 ||
      file_size > max_artifact_bytes ||
      (!workspace_revision_id.isEmpty() &&
       !validStableId(workspace_revision_id)) ||
      (!project_revision_id.isEmpty() && !validStableId(project_revision_id)) ||
      !validHeaderValue(editing_lease, 512) ||
      !validHeaderValue(idempotency_key, 120) ||
      canonical_entity_index.isEmpty() ||
      canonical_entity_index.size() > max_json_response_bytes) {
    handler({}, {0, QStringLiteral("invalid_request_metadata"),
                 tr("The Map Hub snapshot metadata is invalid.")});
    return;
  }
  const auto entity_index_sha256 =
      QString::fromLatin1(QCryptographicHash::hash(canonical_entity_index,
                                                   QCryptographicHash::Sha256)
                              .toHex());
  auto entity_index_payload = canonical_entity_index;
  auto entity_index_encoding = QStringLiteral("identity");
  if (canonical_entity_index.size() >= 512) {
    auto compressed = ZstdCodec::compress(canonical_entity_index);
    if (!compressed.isEmpty() &&
        compressed.size() + 32 < canonical_entity_index.size()) {
      entity_index_payload = std::move(compressed);
      entity_index_encoding = QStringLiteral("zstd");
    }
  }
  auto req = request(
      QStringLiteral("/api/v1/workspaces/%1/snapshots").arg(workspace_id));
  req.setRawHeader("X-Editing-Lease", editing_lease.toUtf8());
  auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
  multi->append(textPart("protocol", QStringLiteral("oom-map-ops/1")));
  multi->append(
      textPart("base_stream_sequence", QString::number(base_stream_sequence)));
  multi->append(textPart("base_stream_hash", base_stream_hash));
  multi->append(textPart("sha256", file_sha256));
  multi->append(textPart("size_bytes", QString::number(file_size)));
  multi->append(textPart("workspace_revision_id", workspace_revision_id));
  multi->append(textPart("project_revision_id", project_revision_id));
  multi->append(textPart("client_instance_id", client_instance_id));
  multi->append(textPart("idempotency_key", idempotency_key));
  multi->append(
      textPart("entity_index_content_encoding", entity_index_encoding));
  multi->append(textPart("entity_index_sha256", entity_index_sha256));

  QHttpPart map_part;
  map_part.setHeader(
      QNetworkRequest::ContentDispositionHeader,
      QStringLiteral(
          "form-data; name=\"file\"; filename=\"workspace-snapshot.omap\""));
  map_part.setHeader(QNetworkRequest::ContentTypeHeader,
                     QStringLiteral("application/vnd.openorienteering.omap"));
  auto *file = new QFile(file_path, multi);
  if (!file->open(QIODevice::ReadOnly) || file->size() != file_size) {
    const auto message =
        file->isOpen()
            ? tr("The local map changed while its snapshot was prepared.")
            : file->errorString();
    delete multi;
    handler({}, {0, QStringLiteral("local_file"), message});
    return;
  }
  map_part.setBodyDevice(file);
  multi->append(map_part);

  QHttpPart index_part;
  index_part.setHeader(
      QNetworkRequest::ContentDispositionHeader,
      QStringLiteral(
          "form-data; name=\"entity_index\"; filename=\"entity-index.json\""));
  index_part.setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("application/json"));
  auto *index_buffer = new QBuffer(multi);
  index_buffer->setData(entity_index_payload);
  index_buffer->open(QIODevice::ReadOnly);
  index_part.setBodyDevice(index_buffer);
  multi->append(index_part);

  auto *reply = network->post(req, multi);
  multi->setParent(reply);
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

void MapHubApiClient::beginMapperConnection(const QString &client_name,
                                            JsonHandler handler) {
  auto name = client_name.trimmed();
  if (name.isEmpty() || name.size() > 80) {
    handler({}, {0, QStringLiteral("invalid_client_name"),
                 tr("Mapper could not create a valid name for this device.")});
    return;
  }
  sendJson("POST", QStringLiteral("/api/v1/auth/mapper/connect"),
           {{QStringLiteral("client_name"), name}}, false, std::move(handler));
}

void MapHubApiClient::exchangeMapperConnection(const QString &request_id,
                                               const QString &device_secret,
                                               JsonHandler handler) {
  if (!validStableId(request_id)) {
    handler({}, invalidIdentifierError());
    return;
  }
  if (device_secret.isEmpty() || device_secret.size() > 200) {
    handler({}, {0, QStringLiteral("invalid_connection_secret"),
                 tr("The Map Hub connection request is invalid.")});
    return;
  }
  sendJson(
      "POST",
      QStringLiteral("/api/v1/auth/mapper/connect/%1/exchange").arg(request_id),
      {{QStringLiteral("device_secret"), device_secret}}, false,
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

void MapHubApiClient::openProject(const QString &project_id,
                                  JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!validStableId(project_id)) {
    handler({}, invalidIdentifierError());
    return;
  }
  finishJson(network->get(request(
                 QStringLiteral("/api/v1/projects/%1/open").arg(project_id))),
             std::move(handler));
}

void MapHubApiClient::createEditAccessRequest(const QString &project_id,
                                              const QString &message,
                                              const QString &idempotency_key,
                                              JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!validStableId(project_id)) {
    handler({}, invalidIdentifierError());
    return;
  }
  if (message.toUtf8().size() > 2000 ||
      !validHeaderValue(idempotency_key, 120)) {
    handler({}, {0, QStringLiteral("invalid_edit_access_request"),
                 tr("The edit-access request or transaction key is invalid.")});
    return;
  }
  auto req = request(QStringLiteral("/api/v1/projects/%1/edit-access-requests")
                         .arg(project_id));
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json"));
  req.setRawHeader("Idempotency-Key", idempotency_key.toUtf8());
  QJsonObject body;
  if (!message.trimmed().isEmpty())
    body.insert(QStringLiteral("message"), message.trimmed());
  finishJson(
      network->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact)),
      std::move(handler));
}

void MapHubApiClient::editAccessRequest(const QString &request_id,
                                        const QString &etag,
                                        SyncStateHandler handler) {
  if (!ensureReady(true, [handler](const QJsonObject &, const Error &error) {
        handler({}, {}, false, error);
      }))
    return;
  if (!validStableId(request_id)) {
    handler({}, {}, false, invalidIdentifierError());
    return;
  }
  auto req = request(
      QStringLiteral("/api/v1/edit-access-requests/%1").arg(request_id));
  if (!etag.isEmpty() && validHeaderValue(etag, 512))
    req.setRawHeader("If-None-Match", etag.toUtf8());
  auto *reply = network->get(req);
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
        const auto response_etag = QString::fromUtf8(reply->rawHeader("ETag"));
        const auto status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 304 && reply->error() == QNetworkReply::NoError) {
          handler({}, response_etag, true, {});
        } else if (*too_large) {
          handler({}, response_etag, false,
                  {status, QStringLiteral("response_too_large"),
                   tr("Map Hub returned more than 16 MiB of edit-access "
                      "state; Mapper discarded the response.")});
        } else {
          MapHubApiClient::Error decoding_error;
          if (!decodeJsonResponse(reply, body.get(), &decoding_error)) {
            handler({}, response_etag, false, decoding_error);
            reply->deleteLater();
            return;
          }
          if (reply->error() != QNetworkReply::NoError) {
            handler({}, response_etag, false, replyError(reply, *body));
            reply->deleteLater();
            return;
          }
          QJsonParseError parse_error;
          const auto document = QJsonDocument::fromJson(*body, &parse_error);
          if (parse_error.error != QJsonParseError::NoError ||
              !document.isObject()) {
            handler({}, response_etag, false,
                    {status, QStringLiteral("invalid_response"),
                     tr("Map Hub returned invalid edit-access state.")});
          } else {
            handler(document.object(), response_etag, false, {});
          }
        }
        reply->deleteLater();
      });
}

void MapHubApiClient::cancelEditAccessRequest(const QString &request_id,
                                              JsonHandler handler) {
  if (!validStableId(request_id)) {
    handler({}, invalidIdentifierError());
    return;
  }
  sendJson(
      "POST",
      QStringLiteral("/api/v1/edit-access-requests/%1/cancel").arg(request_id),
      {}, true, std::move(handler));
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
    const QString &idempotency_key, qint64 stream_sequence,
    const QString &stream_hash, const QString &project_revision_id,
    const QString &entity_index_sha256, JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!validStableId(workspace_id)) {
    handler({}, invalidIdentifierError());
    return;
  }
  static const QRegularExpression hash_pattern(
      QStringLiteral("^[0-9a-f]{64}$"));
  if ((!base_revision_id.isEmpty() && !validStableId(base_revision_id)) ||
      (!project_revision_id.isEmpty() && !validStableId(project_revision_id)) ||
      stream_sequence < 0 || !hash_pattern.match(stream_hash).hasMatch() ||
      !hash_pattern.match(entity_index_sha256).hasMatch() ||
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
  multi->append(textPart("stream_sequence", QString::number(stream_sequence)));
  multi->append(textPart("stream_hash", stream_hash));
  multi->append(textPart("project_revision_id", project_revision_id));
  multi->append(textPart("entity_index_sha256", entity_index_sha256));
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

void MapHubApiClient::workspaceSyncState(const QString &workspace_id,
                                         const QString &etag,
                                         const QString &editing_lease,
                                         SyncStateHandler handler) {
  if (!ensureReady(true, [handler](const QJsonObject &, const Error &error) {
        handler({}, {}, false, error);
      }))
    return;
  if (!validStableId(workspace_id) ||
      (!editing_lease.isEmpty() &&
       !validHeaderValue(editing_lease, 4096))) {
    handler({}, {}, false,
            {0, QStringLiteral("invalid_request_metadata"),
             tr("The workspace identifier or editing lease is invalid.")});
    return;
  }
  auto req = request(
      QStringLiteral("/api/v1/workspaces/%1/sync-state").arg(workspace_id));
  if (!etag.isEmpty() && validHeaderValue(etag, 512))
    req.setRawHeader("If-None-Match", etag.toUtf8());
  if (!editing_lease.isEmpty())
    req.setRawHeader("X-Editing-Lease", editing_lease.toUtf8());
  auto *reply = network->get(req);
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
        const auto response_etag = QString::fromUtf8(reply->rawHeader("ETag"));
        const auto status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 304 && reply->error() == QNetworkReply::NoError) {
          handler({}, response_etag, true, {});
        } else if (*too_large) {
          handler({}, response_etag, false,
                  {status, QStringLiteral("response_too_large"),
                   tr("Map Hub returned more than 16 MiB of synchronization "
                      "state; Mapper discarded the response.")});
        } else {
          MapHubApiClient::Error decoding_error;
          if (!decodeJsonResponse(reply, body.get(), &decoding_error)) {
            handler({}, response_etag, false, decoding_error);
            reply->deleteLater();
            return;
          }
          if (reply->error() != QNetworkReply::NoError) {
            handler({}, response_etag, false, replyError(reply, *body));
            reply->deleteLater();
            return;
          }
          QJsonParseError parse_error;
          auto document = QJsonDocument::fromJson(*body, &parse_error);
          if (parse_error.error != QJsonParseError::NoError ||
              !document.isObject()) {
            handler({}, response_etag, false,
                    {status, QStringLiteral("invalid_response"),
                     tr("Map Hub returned invalid synchronization state.")});
          } else {
            handler(document.object(), response_etag, false, {});
          }
        }
        reply->deleteLater();
      });
}

void MapHubApiClient::workspaceEntityIndex(const QUrl &url,
                                           JsonHandler handler) {
  if (!ensureReady(true, handler))
    return;
  if (!url.isValid() || url.isEmpty() || !isSameOrigin(url)) {
    handler({}, {0, QStringLiteral("untrusted_download"),
                 tr("Map Hub returned an entity-index URL on a different "
                    "origin; the token was not sent.")});
    return;
  }
  finishJson(network->get(request(url)), std::move(handler));
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
  downloadArtifact(url, expected_sha256, -1, destination, std::move(handler));
}

void MapHubApiClient::downloadArtifact(const QUrl &url,
                                       const QString &expected_sha256,
                                       qint64 expected_size,
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
  if (expected_size > max_artifact_bytes) {
    handler({}, {0, QStringLiteral("response_too_large"),
                 tr("The map artifact exceeds Mapper's 2 GiB download "
                    "limit.")});
    return;
  }
  if (!isSameOrigin(url)) {
    handler({}, {0, QStringLiteral("untrusted_download"),
                 tr("Map Hub returned an artifact URL on a different origin; "
                    "the token was not sent.")});
    return;
  }
  auto download_request = request(url);
  download_request.setRawHeader("Accept", "application/octet-stream");
  download_request.setRawHeader("Accept-Encoding", "identity");
  auto *reply = network->get(download_request);
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
  connect(
      reply, &QNetworkReply::metaDataChanged, this,
      [reply, state, expected_size] {
        bool valid = false;
        const auto length = reply->header(QNetworkRequest::ContentLengthHeader)
                                .toLongLong(&valid);
        if (valid &&
            (length > max_artifact_bytes ||
             (expected_size >= 0 && length != expected_size)) &&
            !state->error) {
          state->error = {
              reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                  .toInt(),
              length > max_artifact_bytes ? QStringLiteral("response_too_large")
                                          : QStringLiteral("size_mismatch"),
              length > max_artifact_bytes
                  ? MapHubApiClient::tr("The map artifact exceeds Mapper's 2 "
                                        "GiB download limit.")
                  : MapHubApiClient::tr(
                        "The map artifact response size did not match its "
                        "revision metadata.")};
          reply->abort();
        }
      });
  connect(reply, &QIODevice::readyRead, this, [reply, file, state] {
    consumeArtifactData(reply, file, state, true);
  });
  connect(
      reply, &QNetworkReply::finished, this,
      [reply, file, state, destination, expected_sha256, expected_size,
       handler = std::move(handler)]() mutable {
        consumeArtifactData(reply, file, state, false);
        const auto actual = QString::fromLatin1(state->hash.result().toHex());
        const auto received = state->received;
        const auto state_error = state->error;
        bool header_size_valid = false;
        const auto header_size =
            reply->rawHeader("X-Artifact-Size").toLongLong(&header_size_valid);
        const auto header_sha =
            QString::fromLatin1(reply->rawHeader("X-Artifact-SHA256"));
        delete state;
        if (state_error) {
          file->cancelWriting();
          handler({}, state_error);
        } else if (reply->error() != QNetworkReply::NoError) {
          file->cancelWriting();
          handler({}, replyError(reply, {}));
        } else if (expected_size >= 0 &&
                   (!header_size_valid || header_size != expected_size ||
                    header_sha.compare(expected_sha256, Qt::CaseInsensitive) !=
                        0)) {
          file->cancelWriting();
          handler({},
                  {reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                       .toInt(),
                   QStringLiteral("artifact_metadata_mismatch"),
                   MapHubApiClient::tr(
                       "Map Hub did not bind the download response to the "
                       "requested revision metadata.")});
        } else if (actual.compare(expected_sha256, Qt::CaseInsensitive) != 0) {
          file->cancelWriting();
          handler({},
                  {reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                       .toInt(),
                   QStringLiteral("checksum_mismatch"),
                   MapHubApiClient::tr("The downloaded map did not match the "
                                       "server checksum; it was not opened.")});
        } else if (expected_size >= 0 && received != expected_size) {
          file->cancelWriting();
          handler({},
                  {reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                       .toInt(),
                   QStringLiteral("size_mismatch"),
                   MapHubApiClient::tr(
                       "The downloaded map size did not match the revision "
                       "metadata; it was not opened.")});
        } else if (!file->commit()) {
          handler({}, {0, QStringLiteral("local_file"), file->errorString()});
        } else {
          handler(destination, {});
        }
        reply->deleteLater();
      });
}

} // namespace OpenOrienteering
