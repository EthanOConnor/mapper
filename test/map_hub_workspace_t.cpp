/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_workspace_t.h"

#include <limits>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QtTest>

#include "collaboration/managed_map_workspace.h"
#include "collaboration/map_hub_api_client.h"
#include "collaboration/map_hub_imagery_catalog.h"
#include "collaboration/map_hub_read_only_document.h"
#include "collaboration/map_hub_sync_queue.h"
#include "collaboration/map_hub_workspace.h"
#include "collaboration/oom_json.h"
#include "collaboration/zstd_codec.h"
#include "imagery/oic_catalog.h"

using namespace OpenOrienteering;

void MapHubWorkspaceTest::canonicalizesOperationJsonExactly() {
  const QJsonObject value{
      {QStringLiteral("z"),
       QJsonArray{QString::fromUtf8("é\n"), QJsonValue::Null}},
      {QStringLiteral("a"),
       QJsonObject{{QStringLiteral("n"),
                    QJsonValue(std::numeric_limits<qint64>::max())},
                   {QStringLiteral("b"), true}}},
      {QStringLiteral("xml"),
       QStringLiteral(
           "<object uuid=\"00000000-0000-4000-8000-000000000001\"/>")}};
  QString error;
  const auto bytes = OomJson::canonical(value, &error);
  QVERIFY2(!bytes.isEmpty(), qPrintable(error));
  QCOMPARE(
      bytes,
      QByteArray(
          "{\"a\":{\"b\":true,\"n\":9223372036854775807},\"xml\":\"<object "
          "uuid=\\\"00000000-0000-4000-8000-000000000001\\\"/>\",\"z\":[\""
          "\xc3\xa9\\n\",null]}"));
}

void MapHubWorkspaceTest::rejectsValuesOutsideOperationJsonProfile() {
  QString error;
  QVERIFY(OomJson::canonical(QJsonValue(-1), &error).isEmpty());
  QVERIFY(!error.isEmpty());

  error.clear();
  QString lone_surrogate(1, QChar(0xd800));
  QVERIFY(OomJson::canonical(lone_surrogate, &error).isEmpty());
  QVERIFY(!error.isEmpty());

  error.clear();
  QVERIFY(OomJson::canonical(QJsonObject{{QString::fromUtf8("é"), 1}}, &error)
              .isEmpty());
  QVERIFY(!error.isEmpty());
}

void MapHubWorkspaceTest::boundsZstdTransportFrames() {
  const auto source = QByteArray("semantic-map-operation\n").repeated(4096);
  QString error;
  const auto frame = ZstdCodec::compress(source, 3, &error);
  QVERIFY2(!frame.isEmpty(), qPrintable(error));
  QVERIFY(frame.size() < source.size());
  QCOMPARE(ZstdCodec::decompress(frame, source.size(), &error), source);

  error.clear();
  QVERIFY(ZstdCodec::decompress(frame, source.size() - 1, &error).isEmpty());
  QVERIFY(!error.isEmpty());

  error.clear();
  QVERIFY(ZstdCodec::decompress(frame + frame, source.size() * 2, &error)
              .isEmpty());
  QVERIFY(!error.isEmpty());

  auto corrupt = frame;
  corrupt[0] ^= char(0x40);
  error.clear();
  QVERIFY(ZstdCodec::decompress(corrupt, source.size(), &error).isEmpty());
  QVERIFY(!error.isEmpty());
}

void MapHubWorkspaceTest::initTestCase() {
  QCoreApplication::setOrganizationName(QStringLiteral("OpenOrienteeringTest"));
  QCoreApplication::setApplicationName(QStringLiteral("MapperMapHubTest"));
  QStandardPaths::setTestModeEnabled(true);
  static QTemporaryDir record_directory;
  QVERIFY(record_directory.isValid());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", record_directory.path().toUtf8());
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", record_directory.path().toUtf8());
  qputenv("MAPPER_MAP_HUB_READ_ONLY_ROOT", record_directory.path().toUtf8());
}

void MapHubWorkspaceTest::recordRoundTripsWithoutSecrets() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto map_path = directory.filePath(QStringLiteral("Kelsey Creek.omap"));
  QFile map(map_path);
  QVERIFY(map.open(QIODevice::WriteOnly));
  map.write("omap");
  map.close();

  ManagedMapWorkspace original;
  original.local_map_path = map_path;
  original.server_url = QStringLiteral("https://maps.example.test");
  original.organization_id = QStringLiteral("org-id");
  original.project_id = QStringLiteral("project-id");
  original.project_title = QStringLiteral("Kelsey Creek + Wilburton");
  original.work_package_id = QStringLiteral("package-id");
  original.workspace_id = QStringLiteral("workspace-id");
  original.base_revision_id = QStringLiteral("revision-id");
  original.base_sha256 = QString(64, QLatin1Char('a'));
  original.exclusive_editing = true;
  original.lease_expires_at = QDateTime::currentDateTimeUtc().addSecs(3600);
  QString error;
  QVERIFY2(ManagedMapWorkspace::save(original, &error), qPrintable(error));
  auto loaded = ManagedMapWorkspace::loadForMap(map_path, &error);
  QVERIFY2(loaded.isValid(), qPrintable(error));
  QCOMPARE(loaded.project_id, original.project_id);
  QCOMPARE(loaded.base_sha256, original.base_sha256);
  QCOMPARE(loaded.exclusive_editing, true);
  const auto found = ManagedMapWorkspace::findForWorkspace(
      original.server_url, original.workspace_id, &error);
  QVERIFY2(found.isValid(), qPrintable(error));
  QCOMPARE(QFileInfo(found.local_map_path).canonicalFilePath(),
           QFileInfo(original.local_map_path).canonicalFilePath());

  QFile record(ManagedMapWorkspace::recordPathForMap(map_path));
  QVERIFY(record.open(QIODevice::ReadOnly));
  auto bytes = record.readAll();
  QVERIFY(!bytes.contains("Bearer"));
  QVERIFY(!bytes.contains("lease_token"));
#ifdef Q_OS_UNIX
  QVERIFY(record.permissions().testFlag(QFileDevice::ReadOwner));
  QVERIFY(!record.permissions().testFlag(QFileDevice::ReadGroup));
  QVERIFY(!record.permissions().testFlag(QFileDevice::ReadOther));
#endif
}

void MapHubWorkspaceTest::recordIsBoundToCanonicalMapPath() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto first = directory.filePath(QStringLiteral("first.omap"));
  auto second = directory.filePath(QStringLiteral("second.omap"));
  for (const auto &path : {first, second}) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("x");
  }
  QVERIFY(ManagedMapWorkspace::recordPathForMap(first) !=
          ManagedMapWorkspace::recordPathForMap(second));
}

void MapHubWorkspaceTest::
    readOnlyDocumentRoundTripsAndRejectsPathSubstitution() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto map_path =
      directory.filePath(QStringLiteral("Luther Burbank-r7.omap"));
  const auto other_path = directory.filePath(QStringLiteral("other.omap"));
  for (const auto &path : {map_path, other_path}) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("omap"), qint64(4));
  }

  MapHubReadOnlyDocument original;
  original.local_map_path = map_path;
  original.server_url = QStringLiteral("https://maps.example.test");
  original.organization_id =
      QStringLiteral("10000000-0000-4000-8000-000000000001");
  original.organization_name = QStringLiteral("Cascade Orienteering");
  original.project_id = QStringLiteral("20000000-0000-4000-8000-000000000001");
  original.project_title = QStringLiteral("Luther Burbank Park");
  original.revision_id = QStringLiteral("30000000-0000-4000-8000-000000000007");
  original.revision_number = 7;
  original.revision_sha256 = QString(64, QLatin1Char('a'));
  original.revision_size_bytes = 4;
  original.artifact_kind = QStringLiteral("omap");
  original.artifact_name = QStringLiteral("luther-burbank.omap");
  original.manifest_url =
      QStringLiteral("https://maps.example.test/api/v1/projects/"
                     "20000000-0000-4000-8000-000000000001/manifest");
  original.access_request_id =
      QStringLiteral("40000000-0000-4000-8000-000000000001");
  original.access_request_status = QStringLiteral("pending");
  original.last_checked_at = QDateTime::currentDateTimeUtc();

  QString error;
  QVERIFY2(MapHubReadOnlyDocument::save(original, &error), qPrintable(error));
  const auto loaded = MapHubReadOnlyDocument::loadForMap(map_path, &error);
  QVERIFY2(loaded.isValid(), qPrintable(error));
  QCOMPARE(loaded.project_id, original.project_id);
  QCOMPARE(loaded.revision_id, original.revision_id);
  QCOMPARE(loaded.access_request_status, QStringLiteral("pending"));
  QCOMPARE(loaded.revision_size_bytes, qint64(4));

  QFile record(MapHubReadOnlyDocument::recordPathForMap(map_path));
  QVERIFY(record.open(QIODevice::ReadOnly));
  const auto bytes = record.readAll();
  QVERIFY(!bytes.contains("Bearer"));
  QVERIFY(!bytes.contains("lease"));
#ifdef Q_OS_UNIX
  QVERIFY(record.permissions().testFlag(QFileDevice::ReadOwner));
  QVERIFY(!record.permissions().testFlag(QFileDevice::ReadGroup));
  QVERIFY(!record.permissions().testFlag(QFileDevice::ReadOther));
#endif

  QVERIFY(!MapHubReadOnlyDocument::loadForMap(other_path).isValid());
  QVERIFY(MapHubReadOnlyDocument::recordPathForMap(map_path) !=
          MapHubReadOnlyDocument::recordPathForMap(other_path));
  QVERIFY(MapHubReadOnlyDocument::removeForMap(map_path, &error));
  QVERIFY(
      !QFileInfo::exists(MapHubReadOnlyDocument::recordPathForMap(map_path)));
}

void MapHubWorkspaceTest::validatesServerTransport() {
  QVERIFY(MapHubApiClient::isAcceptableServerUrl(
      QUrl(QStringLiteral("https://maps.example.test"))));
  QVERIFY(MapHubApiClient::isAcceptableServerUrl(
      QUrl(QStringLiteral("http://localhost:8766"))));
  QVERIFY(!MapHubApiClient::isAcceptableServerUrl(
      QUrl(QStringLiteral("http://maps.example.test"))));
  QVERIFY(!MapHubApiClient::isAcceptableServerUrl(
      QUrl(QStringLiteral("https://user:secret@maps.example.test"))));
  QVERIFY(!MapHubApiClient::isAcceptableServerUrl(
      QUrl(QStringLiteral("https://maps.example.test/api"))));
  QVERIFY(!MapHubApiClient::isAcceptableServerUrl(
      QUrl(QStringLiteral("https://maps.example.test/?token=secret"))));
  QVERIFY(!MapHubApiClient::isAcceptableServerUrl(
      QUrl(QStringLiteral("file:///tmp/map"))));
}

void MapHubWorkspaceTest::identifiesMapperWorkspacePackageTypes() {
  for (const auto &package_type :
       {QStringLiteral("basemap"), QStringLiteral("new_mapping"),
        QStringLiteral("remap"), QStringLiteral("update"),
        QStringLiteral("field_check"), QStringLiteral("review")}) {
    QVERIFY2(MapHubApiClient::isMapperWorkspacePackageType(package_type),
             qPrintable(package_type));
  }

  for (const auto &package_type :
       {QStringLiteral("course_design"), QStringLiteral("production"),
        QStringLiteral(""), QStringLiteral("future_package")}) {
    QVERIFY2(!MapHubApiClient::isMapperWorkspacePackageType(package_type),
             qPrintable(package_type));
  }
}

void MapHubWorkspaceTest::classifiesWorkspaceBaselines() {
  using WorkspaceBaseline = MapHubApiClient::WorkspaceBaseline;
  QCOMPARE(MapHubApiClient::classifyWorkspaceBaseline({}),
           WorkspaceBaseline::NoRevision);
  QCOMPARE(MapHubApiClient::classifyWorkspaceBaseline(
               {{QStringLiteral("id"), QStringLiteral("revision-id")}}),
           WorkspaceBaseline::IncompleteRevision);
  QCOMPARE(MapHubApiClient::classifyWorkspaceBaseline(
               {{QStringLiteral("download_url"), QStringLiteral("http://[")}}),
           WorkspaceBaseline::IncompleteRevision);
  QCOMPARE(MapHubApiClient::classifyWorkspaceBaseline(
               {{QStringLiteral("download_url"),
                 QStringLiteral("https://maps.example.test/artifacts/map")}}),
           WorkspaceBaseline::ArtifactReference);
}

void MapHubWorkspaceTest::hashesArtifactsExactly() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  auto path = directory.filePath(QStringLiteral("map.omap"));
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write("abc"), qint64(3));
  file.close();
  QString error;
  QCOMPARE(
      MapHubApiClient::sha256ForFile(path, &error),
      QStringLiteral(
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  QVERIFY(error.isEmpty());
}

void MapHubWorkspaceTest::editAccessUsesNativeIdempotentEndpoints() {
  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost))
    QSKIP("The test sandbox does not permit a loopback listener");

  const auto project_id =
      QStringLiteral("20000000-0000-4000-8000-000000000001");
  const auto request_id =
      QStringLiteral("40000000-0000-4000-8000-000000000001");
  QList<QByteArray> requests;
  connect(&server, &QTcpServer::newConnection, this, [&] {
    auto *socket = server.nextPendingConnection();
    connect(
        socket, &QTcpSocket::readyRead, this,
        [&, socket, bytes = QByteArray{}, responded = false]() mutable {
          if (responded)
            return;
          bytes.append(socket->readAll());
          const auto header_end = bytes.indexOf("\r\n\r\n");
          if (header_end < 0)
            return;
          const QRegularExpression length_pattern(
              QStringLiteral("(?im)^Content-Length:\\s*(\\d+)\\s*$"));
          const auto match =
              length_pattern.match(QString::fromLatin1(bytes.left(header_end)));
          const auto content_length =
              match.hasMatch() ? match.captured(1).toLongLong() : 0;
          if (bytes.size() < header_end + 4 + content_length)
            return;
          responded = true;
          requests.push_back(bytes);
          const auto request_number = requests.size();
          if (request_number == 2) {
            socket->write("HTTP/1.1 304 Not Modified\r\n"
                          "ETag: \"request-v1\"\r\n"
                          "Content-Length: 0\r\n"
                          "Connection: close\r\n\r\n");
          } else {
            const auto status = request_number == 1 ? QByteArray("pending")
                                                    : QByteArray("cancelled");
            const auto response_body =
                QByteArray("{\"schema_version\":1,\"request\":{\"id\":\"") +
                request_id.toUtf8() + QByteArray("\",\"project_id\":\"") +
                project_id.toUtf8() +
                QByteArray("\",\"requested_revision_id\":null,\"status\":\"") +
                status +
                QByteArray("\",\"message\":\"Field work tomorrow\","
                           "\"created_at\":\"2026-07-28T12:00:00Z\","
                           "\"updated_at\":\"2026-07-28T12:00:00Z\","
                           "\"expires_at\":\"2026-08-04T12:00:00Z\","
                           "\"resolved_at\":null,\"already_authorized\":false,"
                           "\"assignment\":null,\"poll_url\":\"https://maps."
                           "example.test/poll\",\"cancel_url\":\"https://maps."
                           "example.test/cancel\"}}");
            socket->write(
                QByteArray("HTTP/1.1 200 OK\r\nContent-Type: "
                           "application/json\r\nETag: \"request-v1\"\r\n"
                           "Content-Length: ") +
                QByteArray::number(response_body.size()) +
                QByteArray("\r\nConnection: close\r\n\r\n") + response_body);
          }
          socket->disconnectFromHost();
        });
  });

  MapHubApiClient client(
      QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()),
      QStringLiteral("test-token"));
  QEventLoop loop;
  MapHubApiClient::Error callback_error;
  bool saw_not_modified = false;
  bool completed = false;
  client.createEditAccessRequest(
      project_id, QStringLiteral("Field work tomorrow"),
      QStringLiteral("edit-access-fixture"),
      [&](const QJsonObject &created,
          const MapHubApiClient::Error &create_error) {
        if (create_error) {
          callback_error = create_error;
          loop.quit();
          return;
        }
        QCOMPARE(created.value(QStringLiteral("request"))
                     .toObject()
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("pending"));
        client.editAccessRequest(
            request_id, QStringLiteral("\"request-v1\""),
            [&](const QJsonObject &, const QString &, bool not_modified,
                const MapHubApiClient::Error &poll_error) {
              if (poll_error) {
                callback_error = poll_error;
                loop.quit();
                return;
              }
              saw_not_modified = not_modified;
              client.cancelEditAccessRequest(
                  request_id, [&](const QJsonObject &cancelled,
                                  const MapHubApiClient::Error &cancel_error) {
                    callback_error = cancel_error;
                    completed = true;
                    QCOMPARE(cancelled.value(QStringLiteral("request"))
                                 .toObject()
                                 .value(QStringLiteral("status"))
                                 .toString(),
                             QStringLiteral("cancelled"));
                    loop.quit();
                  });
            });
      });
  QTimer::singleShot(5000, &loop, &QEventLoop::quit);
  loop.exec();

  QVERIFY2(completed, "Edit-access request sequence timed out");
  QVERIFY2(!callback_error, qPrintable(callback_error.message));
  QVERIFY(saw_not_modified);
  QCOMPARE(requests.size(), 3);
  const QByteArray create_line = QByteArray("POST /api/v1/projects/") +
                                 project_id.toUtf8() +
                                 QByteArray("/edit-access-requests HTTP/1.1");
  const QByteArray poll_line = QByteArray("GET /api/v1/edit-access-requests/") +
                               request_id.toUtf8() + QByteArray(" HTTP/1.1");
  const QByteArray cancel_line =
      QByteArray("POST /api/v1/edit-access-requests/") + request_id.toUtf8() +
      QByteArray("/cancel HTTP/1.1");
  QVERIFY(requests[0].contains(create_line));
  QVERIFY(requests[0].contains("Idempotency-Key: edit-access-fixture"));
  QVERIFY(requests[0].contains("{\"message\":\"Field work tomorrow\"}"));
  QVERIFY(requests[1].contains(poll_line));
  QVERIFY(requests[1].toLower().contains(
      QByteArray("if-none-match: \"request-v1\"")));
  QVERIFY(requests[2].contains(cancel_line));
}

void MapHubWorkspaceTest::verifiedDownloadRequiresBoundRevisionHeaders() {
  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost))
    QSKIP("The test sandbox does not permit a loopback listener");

  const QByteArray artifact("omap");
  const auto artifact_sha = QString::fromLatin1(
      QCryptographicHash::hash(artifact, QCryptographicHash::Sha256).toHex());
  QList<QByteArray> requests;
  connect(&server, &QTcpServer::newConnection, this, [&] {
    auto *socket = server.nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, this,
            [&, socket, bytes = QByteArray{}, responded = false]() mutable {
              if (responded)
                return;
              bytes.append(socket->readAll());
              if (!bytes.contains("\r\n\r\n"))
                return;
              responded = true;
              requests.push_back(bytes);
              const auto header_size =
                  requests.size() == 1 ? QByteArray("4") : QByteArray("5");
              socket->write(
                  QByteArray("HTTP/1.1 200 OK\r\nContent-Type: "
                             "application/octet-stream\r\nContent-Length: 4\r\n"
                             "X-Artifact-SHA256: ") +
                  artifact_sha.toUtf8() + QByteArray("\r\nX-Artifact-Size: ") +
                  header_size + QByteArray("\r\nConnection: close\r\n\r\n") +
                  artifact);
              socket->disconnectFromHost();
            });
  });

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto first_path = directory.filePath(QStringLiteral("verified.omap"));
  const auto second_path = directory.filePath(QStringLiteral("rejected.omap"));
  MapHubApiClient client(
      QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()),
      QStringLiteral("test-token"));
  QEventLoop loop;
  MapHubApiClient::Error first_error;
  MapHubApiClient::Error second_error;
  bool completed = false;
  auto url = QUrl(
      QStringLiteral("http://127.0.0.1:%1/artifact").arg(server.serverPort()));
  client.downloadArtifact(
      url, artifact_sha, artifact.size(), first_path,
      [&](const QString &path, const MapHubApiClient::Error &error) {
        first_error = error;
        if (error) {
          loop.quit();
          return;
        }
        QCOMPARE(path, first_path);
        client.downloadArtifact(
            url, artifact_sha, artifact.size(), second_path,
            [&](const QString &, const MapHubApiClient::Error &error) {
              second_error = error;
              completed = true;
              loop.quit();
            });
      });
  QTimer::singleShot(5000, &loop, &QEventLoop::quit);
  loop.exec();

  QVERIFY2(completed, "Artifact verification sequence timed out");
  QVERIFY2(!first_error, qPrintable(first_error.message));
  QCOMPARE(second_error.code, QStringLiteral("artifact_metadata_mismatch"));
  QFile verified(first_path);
  QVERIFY(verified.open(QIODevice::ReadOnly));
  QCOMPARE(verified.readAll(), artifact);
  QVERIFY(!QFileInfo::exists(second_path));
  QCOMPARE(requests.size(), 2);
  for (const auto &request : std::as_const(requests))
    QVERIFY(request.toLower().contains("accept-encoding: identity"));
}

void MapHubWorkspaceTest::checkpointCarriesStreamProjectionDigest() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto path = directory.filePath(QStringLiteral("checkpoint.omap"));
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write("verified checkpoint bytes"), qint64(25));
  file.close();

  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost))
    QSKIP("The test sandbox does not permit a loopback listener");
  QByteArray request_bytes;
  QEventLoop loop;
  bool completed = false;
  MapHubApiClient::Error callback_error;
  connect(&server, &QTcpServer::newConnection, this, [&] {
    auto *socket = server.nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, this, [&, socket] {
      request_bytes.append(socket->readAll());
      const auto header_end = request_bytes.indexOf("\r\n\r\n");
      if (header_end < 0)
        return;
      const auto headers = request_bytes.left(header_end);
      const QRegularExpression length_pattern(
          QStringLiteral("(?im)^Content-Length:\\s*(\\d+)\\s*$"));
      const auto match = length_pattern.match(QString::fromLatin1(headers));
      if (!match.hasMatch())
        return;
      const auto expected = header_end + 4 + match.captured(1).toLongLong();
      if (request_bytes.size() < expected)
        return;
      const QByteArray response_body =
          "{\"revision_id\":\"70000000-0000-4000-8000-000000000001\","
          "\"number\":2,\"state\":\"checkpoint\","
          "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
          "aaaaaaaaaaaaaaaa\"}";
      socket->write(QByteArray("HTTP/1.1 201 Created\r\nContent-Type: "
                               "application/json\r\nContent-Length: ") +
                    QByteArray::number(response_body.size()) +
                    QByteArray("\r\nConnection: close\r\n\r\n") +
                    response_body);
      socket->disconnectFromHost();
    });
  });

  MapHubApiClient client(
      QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()),
      QStringLiteral("test-token"));
  client.checkpoint(
      QStringLiteral("60000000-0000-4000-8000-000000000001"), path,
      QStringLiteral("60000000-0000-4000-8000-000000000002"),
      QStringLiteral("lease"), QStringLiteral("Checkpoint"),
      QStringLiteral("Field edits"), QStringLiteral("fixture-key"), 42,
      QString(64, QLatin1Char('b')),
      QStringLiteral("60000000-0000-4000-8000-000000000003"),
      QString(64, QLatin1Char('c')),
      [&](const QJsonObject &, const MapHubApiClient::Error &error) {
        callback_error = error;
        completed = true;
        loop.quit();
      });
  QTimer::singleShot(5000, &loop, &QEventLoop::quit);
  loop.exec();
  QVERIFY2(completed, "Checkpoint request timed out");
  QVERIFY2(!callback_error, qPrintable(callback_error.message));
  QVERIFY(request_bytes.contains("Idempotency-Key: fixture-key"));
  QVERIFY(request_bytes.contains("X-Editing-Lease: lease"));
  QVERIFY(request_bytes.contains("name=\"stream_sequence\""));
  QVERIFY(request_bytes.contains("\r\n\r\n42\r\n"));
  QVERIFY(request_bytes.contains("name=\"stream_hash\""));
  QVERIFY(request_bytes.contains(QByteArray(64, 'b')));
  QVERIFY(request_bytes.contains("name=\"project_revision_id\""));
  QVERIFY(request_bytes.contains("60000000-0000-4000-8000-000000000003"));
  QVERIFY(request_bytes.contains("name=\"entity_index_sha256\""));
  QVERIFY(request_bytes.contains(QByteArray(64, 'c')));
  QVERIFY(request_bytes.contains("name=\"change_summary\""));
  QVERIFY(request_bytes.contains("Field edits"));
}

void MapHubWorkspaceTest::snapshotCompressesEntityIndex() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto path = directory.filePath(QStringLiteral("snapshot.omap"));
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  QCOMPARE(file.write("verified snapshot bytes"), qint64(23));
  file.close();
  const auto entity_index =
      QByteArray("{\"padding\":\"") + QByteArray(4096, 'a') + QByteArray("\"}");

  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost))
    QSKIP("The test sandbox does not permit a loopback listener");
  QByteArray request_bytes;
  QEventLoop loop;
  bool completed = false;
  MapHubApiClient::Error callback_error;
  connect(&server, &QTcpServer::newConnection, this, [&] {
    auto *socket = server.nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, this, [&, socket] {
      request_bytes.append(socket->readAll());
      const auto header_end = request_bytes.indexOf("\r\n\r\n");
      if (header_end < 0)
        return;
      const QRegularExpression length_pattern(
          QStringLiteral("(?im)^Content-Length:\\s*(\\d+)\\s*$"));
      const auto match = length_pattern.match(
          QString::fromLatin1(request_bytes.left(header_end)));
      if (!match.hasMatch())
        return;
      const auto expected = header_end + 4 + match.captured(1).toLongLong();
      if (request_bytes.size() < expected)
        return;
      const QByteArray response_body = "{\"protocol\":\"oom-map-ops/1\"}";
      socket->write(QByteArray("HTTP/1.1 201 Created\r\nContent-Type: "
                               "application/json\r\nContent-Length: ") +
                    QByteArray::number(response_body.size()) +
                    QByteArray("\r\nConnection: close\r\n\r\n") +
                    response_body);
      socket->disconnectFromHost();
    });
  });

  MapHubApiClient client(
      QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()),
      QStringLiteral("test-token"));
  client.uploadWorkspaceSnapshot(
      QStringLiteral("60000000-0000-4000-8000-000000000061"), path,
      entity_index, 0, QString(64, QLatin1Char('0')),
      MapHubApiClient::sha256ForFile(path), 23,
      QStringLiteral("50000000-0000-4000-8000-000000000061"),
      QStringLiteral("50000000-0000-4000-8000-000000000062"),
      QStringLiteral("40000000-0000-4000-8000-000000000061"),
      QStringLiteral("lease"), QStringLiteral("snapshot-key"),
      [&](const QJsonObject &, const MapHubApiClient::Error &error) {
        callback_error = error;
        completed = true;
        loop.quit();
      });
  QTimer::singleShot(5000, &loop, &QEventLoop::quit);
  loop.exec();
  QVERIFY2(completed, "Snapshot request timed out");
  QVERIFY2(!callback_error, qPrintable(callback_error.message));
  QVERIFY(request_bytes.contains("name=\"entity_index_content_encoding\""));
  QVERIFY(request_bytes.contains("\r\n\r\nzstd\r\n"));
  QVERIFY(!request_bytes.contains(QByteArray(1024, 'a')));
}

void MapHubWorkspaceTest::transactionPostCompressesSemanticOperations() {
  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost))
    QSKIP("The test sandbox does not permit a loopback listener");
  QByteArray request_bytes;
  QEventLoop loop;
  bool completed = false;
  MapHubApiClient::Error callback_error;
  connect(&server, &QTcpServer::newConnection, this, [&] {
    auto *socket = server.nextPendingConnection();
    connect(socket, &QTcpSocket::readyRead, this, [&, socket] {
      request_bytes.append(socket->readAll());
      const auto header_end = request_bytes.indexOf("\r\n\r\n");
      if (header_end < 0)
        return;
      const QRegularExpression length_pattern(
          QStringLiteral("(?im)^Content-Length:\\s*(\\d+)\\s*$"));
      const auto match = length_pattern.match(
          QString::fromLatin1(request_bytes.left(header_end)));
      if (!match.hasMatch())
        return;
      const auto expected = header_end + 4 + match.captured(1).toLongLong();
      if (request_bytes.size() < expected)
        return;
      const QByteArray response_body = "{\"protocol\":\"oom-map-ops/1\"}";
      socket->write(QByteArray("HTTP/1.1 201 Created\r\nContent-Type: "
                               "application/json\r\nContent-Length: ") +
                    QByteArray::number(response_body.size()) +
                    QByteArray("\r\nConnection: close\r\n\r\n") +
                    response_body);
      socket->disconnectFromHost();
    });
  });

  const auto canonical_json =
      QByteArray("{\"xml\":\"") + QByteArray(4096, 'x') + QByteArray("\"}");
  MapHubApiClient client(
      QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()),
      QStringLiteral("test-token"));
  client.postWorkspaceTransaction(
      QStringLiteral("60000000-0000-4000-8000-000000000071"), canonical_json,
      QStringLiteral("lease"),
      [&](const QJsonObject &, const MapHubApiClient::Error &error) {
        callback_error = error;
        completed = true;
        loop.quit();
      });
  QTimer::singleShot(5000, &loop, &QEventLoop::quit);
  loop.exec();
  QVERIFY2(completed, "Transaction request timed out");
  QVERIFY2(!callback_error, qPrintable(callback_error.message));
  QVERIFY(request_bytes.contains("Content-Encoding: zstd"));
  QVERIFY(request_bytes.contains("X-Editing-Lease: lease"));
  QVERIFY(!request_bytes.contains(QByteArray(1024, 'x')));
}

void MapHubWorkspaceTest::pendingDraftRoundTripsAndCoalesces() {
  auto workspace_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto first_sha = QString(64, QLatin1Char('a'));
  auto first_path = MapHubSyncQueue::snapshotPath(workspace_id, first_sha);
  QVERIFY(QDir().mkpath(QFileInfo(first_path).absolutePath()));
  QFile first(first_path);
  QVERIFY(first.open(QIODevice::WriteOnly));
  QCOMPARE(first.write("first"), qint64(5));
  first.close();

  MapHubPendingDraft draft;
  draft.workspace_id = workspace_id;
  draft.local_map_path = QStringLiteral("/maps/connected.omap");
  draft.snapshot_path = first_path;
  draft.sha256 = first_sha;
  draft.size_bytes = 5;
  draft.map_revision = std::numeric_limits<quint64>::max() - 1;
  draft.expected_workspace_revision_id =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  draft.idempotency_key = MapHubSyncQueue::idempotencyKey(
      draft.workspace_id, draft.expected_workspace_revision_id, draft.sha256);
  const auto published_key = MapHubSyncQueue::idempotencyKey(
      draft.workspace_id, draft.expected_workspace_revision_id, draft.sha256,
      41, QString(64, QLatin1Char('1')), QString(64, QLatin1Char('2')));
  const auto later_published_key = MapHubSyncQueue::idempotencyKey(
      draft.workspace_id, draft.expected_workspace_revision_id, draft.sha256,
      42, QString(64, QLatin1Char('3')), QString(64, QLatin1Char('4')));
  QVERIFY(published_key != draft.idempotency_key);
  QVERIFY(later_published_key != published_key);
  draft.staged_at = QDateTime::currentDateTimeUtc();
  QString error;
  QVERIFY2(MapHubSyncQueue::save(draft, &error), qPrintable(error));
  auto loaded = MapHubSyncQueue::load(workspace_id, &error);
  QVERIFY2(loaded.isValid(), qPrintable(error));
  QCOMPARE(loaded.sha256, draft.sha256);
  QCOMPARE(loaded.map_revision, draft.map_revision);
  QCOMPARE(loaded.idempotency_key, draft.idempotency_key);

  auto second_sha = QString(64, QLatin1Char('b'));
  auto second_path = MapHubSyncQueue::snapshotPath(workspace_id, second_sha);
  QFile second(second_path);
  QVERIFY(second.open(QIODevice::WriteOnly));
  QCOMPARE(second.write("second"), qint64(6));
  second.close();
  draft.snapshot_path = second_path;
  draft.sha256 = second_sha;
  draft.size_bytes = 6;
  ++draft.map_revision;
  draft.idempotency_key = MapHubSyncQueue::idempotencyKey(
      draft.workspace_id, draft.expected_workspace_revision_id, draft.sha256);
  QVERIFY2(MapHubSyncQueue::save(draft, &error), qPrintable(error));
  QVERIFY2(MapHubSyncQueue::pruneSnapshots(workspace_id, second_path, &error),
           qPrintable(error));
  QVERIFY(!QFileInfo::exists(first_path));
  QVERIFY(QFileInfo::exists(second_path));
  QCOMPARE(MapHubSyncQueue::load(workspace_id, &error).sha256, second_sha);
}

void MapHubWorkspaceTest::pendingDraftRejectsMovedOrMissingSnapshots() {
  auto workspace_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  MapHubPendingDraft draft;
  draft.workspace_id = workspace_id;
  draft.local_map_path = QStringLiteral("/maps/connected.omap");
  draft.snapshot_path = QStringLiteral("/tmp/not-content-addressed.omap");
  draft.sha256 = QString(64, QLatin1Char('c'));
  draft.size_bytes = 1;
  draft.map_revision = 1;
  draft.idempotency_key = QStringLiteral("mapper-draft-test");
  draft.staged_at = QDateTime::currentDateTimeUtc();
  QString error;
  QVERIFY(!MapHubSyncQueue::save(draft, &error));
  QVERIFY(!error.isEmpty());
}

void MapHubWorkspaceTest::relocatesStaleIosWorkspaceRoots() {
  const auto current_documents =
      QStringLiteral("/var/mobile/Containers/Data/Application/NEW/Documents");
  QCOMPARE(
      relocatedIosMapHubWorkspaceRoot(
          QStringLiteral(
              "/var/mobile/Containers/Data/Application/OLD/Documents/Mapper "
              "Workspaces"),
          current_documents),
      current_documents + QStringLiteral("/Mapper Workspaces"));
  QCOMPARE(
      relocatedIosMapHubWorkspaceRoot(
          QStringLiteral(
              "/private/var/mobile/Containers/Data/Application/OLD/Documents/"
              "Mapper Workspaces/Kelsey Creek"),
          current_documents),
      current_documents + QStringLiteral("/Mapper Workspaces/Kelsey Creek"));
  QCOMPARE(relocatedIosMapHubWorkspaceRoot(
               QStringLiteral("/Users/mapper/Documents/Mapper Workspaces"),
               current_documents),
           QStringLiteral("/Users/mapper/Documents/Mapper Workspaces"));
}

void MapHubWorkspaceTest::preservesPublishedTileMatrixLimits() {
  QJsonArray limits{
      QJsonObject{{QStringLiteral("tileMatrix"), QStringLiteral("12")},
                  {QStringLiteral("minTileRow"), 1431},
                  {QStringLiteral("maxTileRow"), 1432},
                  {QStringLiteral("minTileCol"), 657},
                  {QStringLiteral("maxTileCol"), 658}},
  };
  QJsonObject manifest{
      {QStringLiteral("id"), QStringLiteral("project-id")},
      {QStringLiteral("current_revision"),
       QJsonObject{{QStringLiteral("number"), 2}}},
      {QStringLiteral("title"), QStringLiteral("Kelsey Creek–Wilburton Hill")},
      {QStringLiteral("tile_layers"),
       QJsonArray{QJsonObject{
           {QStringLiteral("id"), QStringLiteral("intensity")},
           {QStringLiteral("title"), QStringLiteral("All-return intensity")},
           {QStringLiteral("type"), QStringLiteral("raster")},
           {QStringLiteral("url_template"),
            QStringLiteral(
                "https://maps.example.test/api/v1/tiles/{z}/{x}/{y}.png")},
           {QStringLiteral("min_zoom"), 12},
           {QStringLiteral("max_zoom"), 18},
           {QStringLiteral("tile_matrix_limits"), limits},
           {QStringLiteral("source_raster"),
            QJsonObject{
                {QStringLiteral("artifact_id"), QStringLiteral("artifact-id")},
                {QStringLiteral("crs"), QStringLiteral("EPSG:6596")},
                {QStringLiteral("pixel_size"), 0.45720091440182875},
                {QStringLiteral("download_url"),
                 QStringLiteral("https://maps.example.test/api/v1/artifacts/"
                                "artifact-id/download")},
            }},
       }}},
  };
  QString error;
  auto document = MapHubImageryCatalog::catalogDocument(
      manifest,
      QStringLiteral(
          "https://maps.example.test/api/v1/projects/project-id/manifest"),
      &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  auto source =
      document.value(QStringLiteral("sources")).toArray().at(0).toObject();
  QCOMPARE(source.value(QStringLiteral("tileMatrixLimits")).toArray(), limits);
  auto source_raster = source.value(QStringLiteral("extensions"))
                           .toObject()
                           .value(QStringLiteral("org.cascadeoc.maphub"))
                           .toObject()
                           .value(QStringLiteral("sourceRaster"))
                           .toObject();
  QCOMPARE(source_raster.value(QStringLiteral("crs")).toString(),
           QStringLiteral("EPSG:6596"));
  auto result = imagery::OicCatalogReader::read(
      QJsonDocument(document).toJson(QJsonDocument::Compact));
  QVERIFY(result.accepted());
  QCOMPARE(result.supportedSourceCount(), qsizetype(1));
  QCOMPARE(result.catalog.sources.at(0).tile_limit_definitions.size(),
           qsizetype(1));
  QCOMPARE(result.catalog.sources.at(0)
               .extensions.value(QStringLiteral("org.cascadeoc.maphub"))
               .toObject()
               .value(QStringLiteral("sourceRaster"))
               .toObject()
               .value(QStringLiteral("artifact_id"))
               .toString(),
           QStringLiteral("artifact-id"));
}

QTEST_GUILESS_MAIN(MapHubWorkspaceTest)
