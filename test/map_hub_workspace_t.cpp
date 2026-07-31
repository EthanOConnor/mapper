/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_workspace_t.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

#include "collaboration/map_hub_device_authorization.h"
#include "collaboration/managed_map_workspace.h"
#include "collaboration/map_hub_api_client.h"
#include "collaboration/map_hub_imagery_catalog.h"
#include "imagery/oic_catalog.h"

using namespace OpenOrienteering;

namespace {

class DeviceAuthorizationServer final : public QObject {
public:
  DeviceAuthorizationServer() {
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, socket,
                [this, socket] { handle(socket); });
      }
    });
  }

  bool start() { return server.listen(QHostAddress::LocalHost); }
  QString url() const {
    return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
  }
  int requestCount() const { return request_count; }
  bool valid() const { return error.isEmpty(); }
  QString failure() const { return error; }
  QString errorString() const { return server.errorString(); }

private:
  void handle(QTcpSocket *socket) {
    auto request = socket->readAll();
    if (!request.contains("\r\n\r\n"))
      return;
    const auto path = request.left(request.indexOf("\r\n"));
    QByteArray payload;
    if (request_count == 0) {
      if (!path.startsWith("POST /api/v1/auth/mapper/connect HTTP/1.1"))
        error = QStringLiteral("unexpected start request");
      payload = QStringLiteral(
                    R"({"request_id":"11111111-1111-1111-1111-111111111111","device_secret":"a-connection-secret","user_code":"123456","verification_url":"%1/account/mapper/connect/11111111-1111-1111-1111-111111111111/","expires_in":10,"interval":1})")
                    .arg(url())
                    .toUtf8();
      respond(socket, "201 Created", payload);
    } else if (request_count == 1) {
      if (!path.startsWith(
              "POST /api/v1/auth/mapper/connect/11111111-1111-1111-1111-111111111111/exchange HTTP/1.1"))
        error = QStringLiteral("unexpected exchange request");
      respond(socket, "202 Accepted", QByteArrayLiteral(R"({"status":"pending"})"));
    } else if (request_count == 2) {
      respond(socket, "201 Created",
              QByteArrayLiteral(R"({"status":"connected","token":"cocm_connected","organization":{"name":"Cascade Orienteering Club"}})"));
    } else {
      error = QStringLiteral("unexpected extra connection request");
      respond(socket, "500 Internal Server Error", QByteArrayLiteral("{}"));
    }
    ++request_count;
  }

  static void respond(QTcpSocket *socket, const QByteArray &status,
                      const QByteArray &payload) {
    socket->write("HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n"
                  "Content-Length: " + QByteArray::number(payload.size()) +
                  "\r\nConnection: close\r\n\r\n" + payload);
    socket->disconnectFromHost();
  }

  QTcpServer server;
  int request_count = 0;
  QString error;
};

} // namespace

void MapHubWorkspaceTest::initTestCase() {
  QCoreApplication::setOrganizationName(QStringLiteral("OpenOrienteeringTest"));
  QCoreApplication::setApplicationName(QStringLiteral("MapperMapHubTest"));
  QStandardPaths::setTestModeEnabled(true);
  static QTemporaryDir record_directory;
  QVERIFY(record_directory.isValid());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", record_directory.path().toUtf8());
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

void MapHubWorkspaceTest::completesBrowserMediatedConnection() {
  DeviceAuthorizationServer server;
  QVERIFY2(server.start(), qPrintable(server.errorString()));

  MapHubDeviceAuthorization authorization(
      server.url(), QStringLiteral("Mapper test"), this);
  QUrl verification_url;
  QString user_code;
  MapHubDeviceAuthorization::Result result;
  MapHubApiClient::Error error;
  bool complete = false;
  connect(&authorization, &MapHubDeviceAuthorization::verificationRequired,
          this, [&verification_url, &user_code](const QUrl &url,
                                                 const QString &code) {
            verification_url = url;
            user_code = code;
          });
  connect(&authorization, &MapHubDeviceAuthorization::completed, this,
          [&result, &error, &complete](
              const MapHubDeviceAuthorization::Result &connected,
              const MapHubApiClient::Error &connection_error) {
            result = connected;
            error = connection_error;
            complete = true;
          });

  authorization.start();
  QTRY_VERIFY_WITH_TIMEOUT(verification_url.isValid(), 3000);
  QCOMPARE(user_code, QStringLiteral("123456"));
  QCOMPARE(verification_url.host(), QStringLiteral("127.0.0.1"));
  QTRY_VERIFY_WITH_TIMEOUT(complete, 5000);
  QVERIFY2(!error, qPrintable(error.message));
  QCOMPARE(result.token, QStringLiteral("cocm_connected"));
  QCOMPARE(result.organization_name, QStringLiteral("Cascade Orienteering Club"));
  QCOMPARE(server.requestCount(), 3);
  QVERIFY2(server.valid(), qPrintable(server.failure()));
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
            QStringLiteral("https://maps.example.test/api/v1/tiles/{z}/{x}/{y}.png")},
           {QStringLiteral("min_zoom"), 12},
           {QStringLiteral("max_zoom"), 18},
           {QStringLiteral("tile_matrix_limits"), limits},
           {QStringLiteral("source_raster"),
            QJsonObject{
                {QStringLiteral("artifact_id"), QStringLiteral("artifact-id")},
                {QStringLiteral("crs"), QStringLiteral("EPSG:6596")},
                {QStringLiteral("pixel_size"), 0.45720091440182875},
                {QStringLiteral("download_url"),
                 QStringLiteral("https://maps.example.test/api/v1/artifacts/artifact-id/download")},
            }},
       }}},
  };
  QString error;
  auto document = MapHubImageryCatalog::catalogDocument(
      manifest,
      QStringLiteral("https://maps.example.test/api/v1/projects/project-id/manifest"),
      &error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  auto source = document.value(QStringLiteral("sources")).toArray().at(0).toObject();
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
  QCOMPARE(result.catalog.sources.at(0).tile_limit_definitions.size(), qsizetype(1));
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
