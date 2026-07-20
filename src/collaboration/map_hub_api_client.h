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

  explicit MapHubApiClient(QString server_url, QString bearer_token,
                           QObject *parent = nullptr);

  const QUrl &serverUrl() const { return server_url; }
  bool isConfigured() const;
  QString configurationError() const;

  void health(JsonHandler handler);
  void library(JsonHandler handler);
  void projectManifest(const QString &project_id, JsonHandler handler);
  void createProject(const QJsonObject &project, const QString &idempotency_key,
                     JsonHandler handler);
  void startAssignment(const QString &assignment_id, JsonHandler handler);
  void checkpoint(const QString &workspace_id, const QString &file_path,
                  const QString &base_revision_id, const QString &editing_lease,
                  const QString &label, const QString &change_summary,
                  const QString &idempotency_key, JsonHandler handler);
  void submitRevision(const QString &revision_id, const QString &editing_lease,
                      JsonHandler handler);
  void renewLease(const QString &workspace_id, const QString &editing_lease,
                  JsonHandler handler);
  void downloadArtifact(const QUrl &url, const QString &expected_sha256,
                        const QString &destination, DownloadHandler handler);

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
