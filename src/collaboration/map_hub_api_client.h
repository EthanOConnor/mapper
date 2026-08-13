/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_API_CLIENT_H
#define OPENORIENTEERING_MAP_HUB_API_CLIENT_H

#include <functional>

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace OpenOrienteering {

class MapHubApiClient : public QObject {
  Q_OBJECT
public:
  enum class WorkspaceBaseline {
    NoRevision,
    ArtifactReference,
    IncompleteRevision,
  };

  struct Error {
    int http_status = 0;
    QString code;
    QString message;

    explicit operator bool() const { return !message.isEmpty(); }
  };

  using JsonHandler = std::function<void(const QJsonObject &, const Error &)>;
  using DownloadHandler = std::function<void(const QString &, const Error &)>;
  using SyncStateHandler =
      std::function<void(const QJsonObject &, const QString &etag,
                         bool not_modified, const Error &)>;

  explicit MapHubApiClient(QString server_url, QString bearer_token,
                           QObject *parent = nullptr);

  const QUrl &serverUrl() const { return server_url; }
  bool isConfigured() const;
  QString configurationError() const;

  void health(JsonHandler handler);
  void beginMapperConnection(const QString &client_name, JsonHandler handler);
  void exchangeMapperConnection(const QString &request_id,
                                const QString &device_secret,
                                JsonHandler handler);
  void library(JsonHandler handler);
  void projectManifest(const QString &project_id, JsonHandler handler);
  void openProject(const QString &project_id, JsonHandler handler);
  void createEditAccessRequest(const QString &project_id,
                               const QString &message,
                               const QString &idempotency_key,
                               JsonHandler handler);
  void editAccessRequest(const QString &request_id, const QString &etag,
                         SyncStateHandler handler);
  void cancelEditAccessRequest(const QString &request_id, JsonHandler handler);
  void createProject(const QJsonObject &project, const QString &idempotency_key,
                     JsonHandler handler);
  void startAssignment(const QString &assignment_id, JsonHandler handler);
  void checkpoint(const QString &workspace_id, const QString &file_path,
                  const QString &base_revision_id, const QString &editing_lease,
                  const QString &label, const QString &change_summary,
                  const QString &idempotency_key, qint64 stream_sequence,
                  const QString &stream_hash,
                  const QString &project_revision_id,
                  const QString &entity_index_sha256, JsonHandler handler);
  void checkpointFile(const QString &workspace_id, const QString &file_path,
                      const QString &base_revision_id,
                      const QString &file_version_id,
                      const QString &client_instance_id, const QString &label,
                      const QString &change_summary,
                      const QString &idempotency_key, JsonHandler handler);
  void submitRevision(const QString &revision_id, const QString &editing_lease,
                      JsonHandler handler);
  void renewLease(const QString &workspace_id, const QString &editing_lease,
                  JsonHandler handler);
  void workspaceSyncState(const QString &workspace_id, const QString &etag,
                          const QString &editing_lease,
                          SyncStateHandler handler);
  void workspaceFiles(const QString &workspace_id, const QString &etag,
                      const QString &client_instance_id,
                      SyncStateHandler handler);
  void uploadWorkspaceFile(const QString &workspace_id,
                           const QString &file_path,
                           const QString &base_version_id,
                           const QString &file_sha256,
                           const QString &client_instance_id,
                           const QString &device_name,
                           const QString &idempotency_key, JsonHandler handler);
  void workspaceOperations(const QString &workspace_id, qint64 after_sequence,
                           int limit, JsonHandler handler);
  void workspaceEntityIndex(const QUrl &url, JsonHandler handler);
  void postWorkspaceTransaction(const QString &workspace_id,
                                const QByteArray &canonical_json,
                                const QString &editing_lease,
                                JsonHandler handler);
  void acknowledgeWorkspaceOperations(const QString &workspace_id,
                                      const QJsonObject &acknowledgement,
                                      const QString &editing_lease,
                                      JsonHandler handler);
  void uploadWorkspaceSnapshot(
      const QString &workspace_id, const QString &file_path,
      const QByteArray &canonical_entity_index, qint64 base_stream_sequence,
      const QString &base_stream_hash, const QString &file_sha256,
      qint64 file_size, const QString &workspace_revision_id,
      const QString &project_revision_id, const QString &client_instance_id,
      const QString &editing_lease, const QString &idempotency_key,
      JsonHandler handler);
  void downloadArtifact(const QUrl &url, const QString &expected_sha256,
                        const QString &destination, DownloadHandler handler);
  void downloadArtifact(const QUrl &url, const QString &expected_sha256,
                        qint64 expected_size, const QString &destination,
                        DownloadHandler handler);

  static QString sha256ForFile(const QString &path, QString *error = nullptr);
  static bool isAcceptableServerUrl(const QUrl &url);
  static bool isMapperWorkspacePackageType(const QString &package_type);
  static WorkspaceBaseline
  classifyWorkspaceBaseline(const QJsonObject &revision);

signals:
  void downloadProgress(qint64 received, qint64 total);

private:
  QNetworkRequest request(const QString &relative_path,
                          bool authenticated = true) const;
  QNetworkRequest request(const QUrl &url, bool authenticated = true) const;
  void finishJson(QNetworkReply *reply, JsonHandler handler);
  void sendJson(const QByteArray &method, const QString &relative_path,
                const QJsonObject &body, bool authenticated,
                JsonHandler handler);
  bool ensureReady(bool authenticated, const JsonHandler &handler) const;
  bool isSameOrigin(const QUrl &url) const;
  static Error replyError(QNetworkReply *reply, const QByteArray &body);

  QUrl server_url;
  QString bearer_token;
  QNetworkAccessManager *network;
};

} // namespace OpenOrienteering

#endif
