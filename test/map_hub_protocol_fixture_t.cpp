/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_protocol_fixture_t.h"

#include <algorithm>
#include <functional>

#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <QXmlStreamReader>

#include "collaboration/managed_map_workspace.h"
#include "collaboration/map_hub_credentials.h"
#include "collaboration/map_hub_edit_transaction.h"
#include "collaboration/map_hub_entity_index.h"
#include "collaboration/map_hub_operation_store.h"
#include "collaboration/map_hub_sync_controller.h"
#include "collaboration/map_hub_sync_queue.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "core/symbols/point_symbol.h"
#include "core/symbols/symbol.h"
#include "fileformats/xml_file_format_p.h"
#include "test_config.h"
#include "undo/object_undo.h"
#include "undo/undo.h"

using namespace OpenOrienteering;

namespace {

class FixtureHttpServer final : public QObject {
public:
  using Handler = std::function<void(const QByteArray &, QTcpSocket *)>;

  FixtureHttpServer() {
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (auto *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket,
                [this, socket, bytes = QByteArray{}, handled = false]() mutable {
                  if (handled)
                    return;
                  bytes.append(socket->readAll());
                  const auto header_end = bytes.indexOf("\r\n\r\n");
                  if (header_end < 0)
                    return;
                  const QRegularExpression length_pattern(
                      QStringLiteral("(?im)^Content-Length:\\s*(\\d+)\\s*$"));
                  const auto match = length_pattern.match(
                      QString::fromLatin1(bytes.left(header_end)));
                  const auto content_length =
                      match.hasMatch() ? match.captured(1).toLongLong() : 0;
                  if (bytes.size() < header_end + 4 + content_length)
                    return;
                  handled = true;
                  if (handler)
                    handler(bytes, socket);
                });
      }
    });
  }

  bool start() { return server.listen(QHostAddress::LocalHost); }
  QString url() const {
    return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
  }
  QString errorString() const { return server.errorString(); }
  void setHandler(Handler new_handler) { handler = std::move(new_handler); }

  static void respond(QTcpSocket *socket, int status,
                      const QByteArray &body = {},
                      const QByteArray &extra_headers = {}) {
    const auto reason = status == 304 ? QByteArrayLiteral("Not Modified")
                                      : QByteArrayLiteral("OK");
    socket->write(QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                  QByteArray(" ") + reason +
                  QByteArray("\r\nContent-Type: application/json\r\n") +
                  extra_headers +
                  QByteArray("Content-Length: ") +
                  QByteArray::number(body.size()) +
                  QByteArray("\r\nConnection: close\r\n\r\n") + body);
    socket->disconnectFromHost();
  }

private:
  QTcpServer server;
  Handler handler;
};

QJsonObject requestBody(const QByteArray &request) {
  const auto header_end = request.indexOf("\r\n\r\n");
  if (header_end < 0)
    return {};
  return QJsonDocument::fromJson(request.mid(header_end + 4)).object();
}

ManagedMapWorkspace fixtureWorkspace(const QString &map_path,
                                     const QString &server_url,
                                     const QString &workspace_id) {
  ManagedMapWorkspace workspace;
  workspace.local_map_path = map_path;
  workspace.server_url = server_url;
  workspace.project_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  workspace.work_package_id =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  workspace.workspace_id = workspace_id;
  workspace.base_revision_id =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  workspace.active_revision_id = workspace.base_revision_id;
  workspace.project_revision_id =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  workspace.stream_protocol = QStringLiteral("oom-map-ops/1");
  workspace.stream_head_hash = QString(64, QLatin1Char('0'));
  workspace.minimum_available_sequence = 1;
  workspace.status = QStringLiteral("active");
  workspace.sync_etag = QStringLiteral("\"fixture-v1\"");
  return workspace;
}

QByteArray syncStateResponse(const ManagedMapWorkspace &workspace,
                             const QString &status,
                             const QJsonArray &presence = {},
                             int presence_ttl_seconds = 90) {
  const QJsonObject response{
      {QStringLiteral("canonical_json"), QStringLiteral("oom-json/1")},
      {QStringLiteral("protocol"), QStringLiteral("oom-map-ops/1")},
      {QStringLiteral("server_time"),
       QStringLiteral("2026-08-10T20:00:00Z")},
      {QStringLiteral("sync"),
       QJsonObject{{QStringLiteral("poll_after_ms"), 30'000},
                   {QStringLiteral("idle_poll_after_ms"), 120'000},
                   {QStringLiteral("presence_ttl_seconds"),
                    presence_ttl_seconds}}},
      {QStringLiteral("presence"), presence},
      {QStringLiteral("workspace"),
       QJsonObject{{QStringLiteral("status"), status}}},
      {QStringLiteral("workspace_revision"),
       QJsonObject{{QStringLiteral("id"), workspace.active_revision_id}}},
      {QStringLiteral("project_revision"),
       QJsonObject{{QStringLiteral("id"), workspace.project_revision_id}}},
      {QStringLiteral("stream"),
       QJsonObject{{QStringLiteral("head_sequence"), 0},
                   {QStringLiteral("head_hash"),
                    QString(64, QLatin1Char('0'))},
                   {QStringLiteral("minimum_available_sequence"), 1},
                   {QStringLiteral("initial_snapshot_required"), true},
                   {QStringLiteral("uncompacted_operations"), 0},
                   {QStringLiteral("compaction_recommended"), false},
                   {QStringLiteral("compaction_required"), false}}},
      {QStringLiteral("lease"),
       QJsonObject{{QStringLiteral("valid"), true},
                   {QStringLiteral("expires_at"),
                    QStringLiteral("2026-08-11T20:00:00Z")}}},
  };
  return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

} // namespace

void MapHubProtocolFixtureTest::initTestCase() {
  QCoreApplication::setOrganizationName(QStringLiteral("OpenOrienteeringTest"));
  QCoreApplication::setApplicationName(
      QStringLiteral("MapperMapHubProtocolFixtureTest"));
  QStandardPaths::setTestModeEnabled(true);
}

void MapHubProtocolFixtureTest::operationStoreHonorsRequestedClientIdentity() {
  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto prior_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
  const auto workspace_id =
      QStringLiteral("60000000-0000-4000-8000-000000000090");
  const auto client_instance_id =
      QStringLiteral("40000000-0000-4000-8000-000000000090");
  const auto other_client_instance_id =
      QStringLiteral("40000000-0000-4000-8000-000000000099");

  QString error;
  MapHubOperationStore store;
  QVERIFY2(store.open(workspace_id, client_instance_id, &error),
           qPrintable(error));
  QCOMPARE(store.state(&error).client_instance_id, client_instance_id);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  store.close();

  QVERIFY(!store.open(workspace_id, other_client_instance_id, &error));
  QVERIFY(!error.isEmpty());
  error.clear();
  QVERIFY2(store.open(workspace_id, client_instance_id, &error),
           qPrintable(error));
  QCOMPARE(store.state(&error).client_instance_id, client_instance_id);

  if (prior_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_root);
}

void MapHubProtocolFixtureTest::rejectsMalformedEntityIndexes() {
  MapHubEntityIndex index;
  index.stream_hash = QString(64, QLatin1Char('0'));
  index.entities = {
      {QStringLiteral("part"),
       QStringLiteral("10000000-0000-4000-8000-000000000001"),
       1,
       false,
       {},
       {}},
      {QStringLiteral("object"),
       QStringLiteral("20000000-0000-4000-8000-000000000001"),
       1,
       false,
       QStringLiteral("10000000-0000-4000-8000-000000000001"),
       {}},
      {QStringLiteral("object"),
       QStringLiteral("20000000-0000-4000-8000-000000000002"),
       2,
       true,
       {},
       {}},
  };
  QString error;
  QVERIFY2(index.isValid(&error), qPrintable(error));

  index.entities[2].parent_id =
      QStringLiteral("10000000-0000-4000-8000-000000000001");
  QVERIFY(!index.isValid(&error));
  index.entities[2].parent_id.clear();
  index.entities[2].tombstone = false;
  index.entities[2].after_id =
      QStringLiteral("20000000-0000-4000-8000-000000000002");
  QVERIFY(!index.isValid(&error));
}

void MapHubProtocolFixtureTest::nativeBootstrapFixture() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  const auto fixture_directory = test_directory.filePath(
      QStringLiteral("data/protocol/connected-editing-v1"));
  const auto omap_path =
      QDir(fixture_directory).filePath(QStringLiteral("bootstrap.omap"));
  const auto index_path =
      QDir(fixture_directory).filePath(QStringLiteral("bootstrap-index.json"));
  const auto transaction_path =
      QDir(fixture_directory)
          .filePath(QStringLiteral("transaction-combined-1.json"));
  const auto undo_transaction_path =
      QDir(fixture_directory)
          .filePath(QStringLiteral("transaction-undo-2.json"));
  const auto projection_1_path =
      QDir(fixture_directory).filePath(QStringLiteral("projection-1.json"));
  const auto projection_2_path =
      QDir(fixture_directory).filePath(QStringLiteral("projection-2.json"));

  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));
  const auto index = MapHubEntityIndex::bootstrap(map);
  QString index_error;
  const auto index_bytes = index.canonicalBytes(&index_error);
  QVERIFY2(!index_bytes.isEmpty(), qPrintable(index_error));
  QCOMPARE(index.entities.size(), 7);
  QVERIFY2(index.matchesMapTopology(map, &index_error),
           qPrintable(index_error));
  map.moveSymbol(0, map.getNumSymbols());
  QVERIFY(!index.matchesMapTopology(map, &index_error));
  map.moveSymbol(map.getNumSymbols() - 1, 0);
  QVERIFY2(index.matchesMapTopology(map, &index_error),
           qPrintable(index_error));

  if (qEnvironmentVariableIntValue("MAPPER_REGENERATE_PROTOCOL_FIXTURES") ==
      1) {
    QVERIFY(QDir().mkpath(fixture_directory));
    XMLFileExporter exporter(omap_path, &map, nullptr);
    QVERIFY(exporter.doExport());
    QFile index_file(index_path);
    QVERIFY(index_file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(index_file.write(index_bytes), index_bytes.size());
    index_file.close();
  }

  QFile expected_index(index_path);
  QVERIFY2(expected_index.open(QIODevice::ReadOnly), qPrintable(index_path));
  const auto expected_index_bytes = expected_index.readAll();
  QCOMPARE(expected_index_bytes, index_bytes);
  QCOMPARE(MapHubEntityIndex::fromCanonicalBytes(expected_index_bytes)
               .canonicalBytes(),
           expected_index_bytes);

  Map round_trip;
  XMLFileImporter round_trip_importer(omap_path, &round_trip, nullptr);
  QVERIFY2(round_trip_importer.doImport(), qPrintable(omap_path));
  QCOMPARE(MapHubEntityIndex::bootstrap(round_trip).canonicalBytes(),
           index_bytes);
  QCOMPARE(round_trip.getNumParts(), 1);
  QCOMPARE(round_trip.getNumSymbols(), 3);
  QCOMPARE(round_trip.getNumObjects(), 3);

  auto *part = round_trip.getPart(0);
  auto replace = std::make_unique<ReplaceObjectsUndoStep>(&round_trip);
  replace->addObject(1, part->getObject(1)->duplicate());
  part->getObject(1)->setTag(QStringLiteral("fixture"),
                             QStringLiteral("replaced"));

  auto restore_deleted = std::make_unique<AddObjectsUndoStep>(&round_trip);
  auto *deleted = part->releaseObject(2);
  restore_deleted->addObject(2, deleted);

  const auto created_xml = QStringLiteral(
      "<object uuid=\"30000000-0000-4000-8000-000000000004\" "
      "type=\"0\" symbol=\"0\" rotation=\"0.25\"><coords count=\"1\">"
      "22000 -9000;</coords></object>");
  SymbolDictionary symbols;
  for (int i = 0; i < round_trip.getNumSymbols(); ++i)
    symbols.insert(i, round_trip.getSymbol(i));
  QXmlStreamReader xml(created_xml);
  QVERIFY(xml.readNextStartElement());
  std::unique_ptr<Object> created(Object::load(xml, &round_trip, symbols));
  QCOMPARE(created->persistentId(),
           QStringLiteral("30000000-0000-4000-8000-000000000004"));
  part->addObject(created.release());
  auto remove_created = std::make_unique<DeleteObjectsUndoStep>(&round_trip);
  remove_created->addObject(part->getNumObjects() - 1);

  CombinedUndoStep combined(&round_trip);
  combined.push(replace.release());
  combined.push(restore_deleted.release());
  combined.push(remove_created.release());
  QHash<QString, qint64> versions;
  for (const auto &entity : index.entities)
    versions.insert(entity.id, entity.version);
  QString transaction_error;
  auto transaction = MapHubEditTransaction::fromUndoStep(
      round_trip, combined, versions,
      QStringLiteral("40000000-0000-4000-8000-000000000001"), 1, 0,
      QString(64, QLatin1Char('0')),
      QStringLiteral("50000000-0000-4000-8000-000000000001"),
      QStringLiteral("50000000-0000-4000-8000-000000000002"),
      &transaction_error);
  transaction.transaction_id =
      QStringLiteral("40000000-0000-4000-8000-000000000002");
  const auto transaction_bytes = transaction.canonicalBytes(&transaction_error);
  QVERIFY2(!transaction_bytes.isEmpty(), qPrintable(transaction_error));
  QCOMPARE(transaction.operations.size(), 3);
  QCOMPARE(
      transaction.payloadSha256(),
      QStringLiteral(
          "caadb782755ad5c6413728904d976204ccfc79703fb66ba585a70723280b7415"));
  if (qEnvironmentVariableIntValue("MAPPER_REGENERATE_PROTOCOL_FIXTURES") ==
      1) {
    QFile transaction_file(transaction_path);
    QVERIFY(transaction_file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(transaction_file.write(transaction_bytes),
             transaction_bytes.size());
  }
  QFile expected_transaction(transaction_path);
  QVERIFY2(expected_transaction.open(QIODevice::ReadOnly),
           qPrintable(transaction_path));
  QCOMPARE(expected_transaction.readAll(), transaction_bytes);

  QFile projection_1_file(projection_1_path);
  QVERIFY2(projection_1_file.open(QIODevice::ReadOnly),
           qPrintable(projection_1_path));
  const auto projection_1_bytes = projection_1_file.readAll();
  const auto projection_1 = MapHubEntityIndex::fromCanonicalBytes(
      projection_1_bytes, &transaction_error);
  QVERIFY2(projection_1.isValid(&transaction_error),
           qPrintable(transaction_error));
  QHash<QString, qint64> projection_1_versions;
  for (const auto &entity : projection_1.entities)
    projection_1_versions.insert(entity.id, entity.version);

  std::unique_ptr<UndoStep> undo_step(combined.undo());
  QVERIFY(undo_step);
  auto undo_transaction = MapHubEditTransaction::fromUndoStep(
      round_trip, *undo_step, projection_1_versions,
      QStringLiteral("40000000-0000-4000-8000-000000000001"), 2, 1,
      QStringLiteral(
          "e6ff48d917d96081c9bd2929da23cf54abba1a223077516c8dba33a4d5ae4c2b"),
      QStringLiteral("50000000-0000-4000-8000-000000000001"),
      QStringLiteral("50000000-0000-4000-8000-000000000002"),
      &transaction_error);
  undo_transaction.transaction_id =
      QStringLiteral("40000000-0000-4000-8000-000000000003");
  const auto undo_transaction_bytes =
      undo_transaction.canonicalBytes(&transaction_error);
  QVERIFY2(!undo_transaction_bytes.isEmpty(), qPrintable(transaction_error));
  QCOMPARE(
      undo_transaction.payloadSha256(),
      QStringLiteral(
          "7b2bd3179e5d7ce6ebdf516740c8c00d1e8d7cbc7d1e9606cfcae7387a962e65"));
  QFile expected_undo_transaction(undo_transaction_path);
  QVERIFY2(expected_undo_transaction.open(QIODevice::ReadOnly),
           qPrintable(undo_transaction_path));
  QCOMPARE(expected_undo_transaction.readAll(), undo_transaction_bytes);

  QTemporaryDir operation_directory;
  QVERIFY(operation_directory.isValid());
  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", operation_directory.path().toUtf8());
  MapHubOperationStore store;
  QString projection_error;
  QVERIFY2(store.open(QStringLiteral("60000000-0000-4000-8000-000000000001"),
                      &projection_error),
           qPrintable(projection_error));
  QVERIFY2(store.seedInitialProjection(map, &projection_error),
           qPrintable(projection_error));
  MapHubCommittedTransaction committed;
  committed.transaction = transaction;
  committed.stream_sequence = 1;
  committed.payload_sha256 = transaction.payloadSha256(&projection_error);
  committed.stream_hash = MapHubOperationStore::chainHash(
      transaction.expected_stream_hash, committed.payload_sha256);
  QCOMPARE(
      committed.stream_hash,
      QStringLiteral(
          "e6ff48d917d96081c9bd2929da23cf54abba1a223077516c8dba33a4d5ae4c2b"));
  QVERIFY2(store.rebaseOnto({committed}, &projection_error),
           qPrintable(projection_error));
  QCOMPARE(store.entityIndex(&projection_error).canonicalBytes(),
           projection_1_bytes);
  MapHubCommittedTransaction committed_undo;
  committed_undo.transaction = undo_transaction;
  committed_undo.stream_sequence = 2;
  committed_undo.payload_sha256 =
      undo_transaction.payloadSha256(&projection_error);
  committed_undo.stream_hash = MapHubOperationStore::chainHash(
      committed.stream_hash, committed_undo.payload_sha256);
  QCOMPARE(
      committed_undo.stream_hash,
      QStringLiteral(
          "212cc13d6411565a379a030aca679266e96eab8bd839c9df42de1b30bd23f38a"));
  QVERIFY2(store.rebaseOnto({committed_undo}, &projection_error),
           qPrintable(projection_error));
  QFile projection_2_file(projection_2_path);
  QVERIFY2(projection_2_file.open(QIODevice::ReadOnly),
           qPrintable(projection_2_path));
  QCOMPARE(store.entityIndex(&projection_error).canonicalBytes(),
           projection_2_file.readAll());
  MapHubOperationStore restored_store;
  QVERIFY2(restored_store.open(
               QStringLiteral("60000000-0000-4000-8000-000000000002"),
               &projection_error),
           qPrintable(projection_error));
  const auto restored_index = MapHubEntityIndex::fromCanonicalBytes(
      expected_index_bytes, &projection_error);
  QVERIFY2(restored_store.replaceProjection(restored_index, &projection_error),
           qPrintable(projection_error));
  QCOMPARE(restored_store.entityIndex(&projection_error).canonicalBytes(),
           expected_index_bytes);
  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
}

void MapHubProtocolFixtureTest::supportsAllAddressableEntityOperations() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));

  const auto *part = map.getPart(0);
  const auto *symbol = map.getSymbol(0);
  MapHubEditTransaction transaction;
  transaction.client_instance_id =
      QStringLiteral("40000000-0000-4000-8000-000000000011");
  transaction.client_sequence = 1;
  transaction.transaction_id =
      QStringLiteral("40000000-0000-4000-8000-000000000012");
  transaction.expected_stream_hash = QString(64, QLatin1Char('0'));
  transaction.expected_workspace_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000011");
  transaction.expected_project_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000012");
  transaction.operations = {
      {MapHubEditOperation::Kind::PutPart,
       part->persistentId(),
       {},
       {},
       1,
       QStringLiteral("Renamed field sheet")},
      {MapHubEditOperation::Kind::PutSymbol,
       symbol->persistentId(),
       {},
       {},
       1,
       MapHubEditTransaction::symbolFragment(*symbol, map)},
  };
  QString error;
  const auto canonical = transaction.canonicalBytes(&error);
  QVERIFY2(!canonical.isEmpty(), qPrintable(error));
  const auto parsed = MapHubEditTransaction::fromJson(
      QJsonDocument::fromJson(canonical).object(), &error);
  QVERIFY2(parsed.isValid(&error), qPrintable(error));
  QCOMPARE(parsed.operations.size(), 2);
  QCOMPARE(parsed.operations[0].kind, MapHubEditOperation::Kind::PutPart);
  QCOMPARE(parsed.operations[1].kind, MapHubEditOperation::Kind::PutSymbol);

  QTemporaryDir operation_directory;
  QVERIFY(operation_directory.isValid());
  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", operation_directory.path().toUtf8());
  MapHubOperationStore store;
  QVERIFY2(store.open(QStringLiteral("60000000-0000-4000-8000-000000000011"),
                      &error),
           qPrintable(error));
  QVERIFY2(store.seedInitialProjection(map, &error), qPrintable(error));
  MapHubCommittedTransaction committed;
  committed.transaction = transaction;
  committed.stream_sequence = 1;
  committed.payload_sha256 = transaction.payloadSha256(&error);
  committed.stream_hash = MapHubOperationStore::chainHash(
      transaction.expected_stream_hash, committed.payload_sha256);
  committed.committed_at = QStringLiteral("2026-07-28T12:00:00+00:00");
  QVERIFY2(store.rebaseOnto({committed}, &error), qPrintable(error));
  const auto unapplied = store.unappliedTransactions(&error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(unapplied.size(), 1);
  QCOMPARE(unapplied.front().transaction.canonicalBytes(), canonical);
  const auto versions = store.entityVersions(&error);
  QCOMPARE(versions.value(part->persistentId()), qint64(2));
  QCOMPARE(versions.value(symbol->persistentId()), qint64(2));
  QVERIFY2(store.markTransactionsApplied(1, &error), qPrintable(error));
  QVERIFY(store.unappliedTransactions(&error).isEmpty());

  MapHubOperationStore accepted_outbox;
  QVERIFY2(accepted_outbox.open(
               QStringLiteral("60000000-0000-4000-8000-000000000014"), &error),
           qPrintable(error));
  QVERIFY2(accepted_outbox.seedInitialProjection(map, &error),
           qPrintable(error));
  auto accepted_transaction = transaction;
  accepted_transaction.client_instance_id =
      accepted_outbox.state(&error).client_instance_id;
  accepted_transaction.transaction_id =
      QStringLiteral("40000000-0000-4000-8000-000000000014");
  QVERIFY2(accepted_outbox.enqueue(accepted_transaction, &error),
           qPrintable(error));
  const auto accepted_pending = accepted_outbox.nextPending(&error);
  QVERIFY2(accepted_outbox.acknowledge(
               accepted_pending.client_sequence,
               accepted_pending.predicted_stream_sequence,
               accepted_pending.predicted_stream_hash, &error),
           qPrintable(error));
  QCOMPARE(accepted_outbox.pendingCount(&error), 0);
  QCOMPARE(accepted_outbox.unappliedTransactions(&error).size(), 1);
  QVERIFY2(accepted_outbox.markTransactionsApplied(1, &error),
           qPrintable(error));
  QVERIFY(accepted_outbox.unappliedTransactions(&error).isEmpty());

  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
}

void MapHubProtocolFixtureTest::journalsReplayAndRebasesCompactedPendingWork() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));
  const auto bootstrap = MapHubEntityIndex::bootstrap(map);
  const auto *part = map.getPart(0);
  const auto *object = part->getObject(0);

  auto make_pending = [&](const QString &client_instance_id) {
    MapHubEditTransaction transaction;
    transaction.client_instance_id = client_instance_id;
    transaction.client_sequence = 1;
    transaction.transaction_id =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    transaction.expected_stream_hash = QString(64, QLatin1Char('0'));
    transaction.expected_workspace_revision_id =
        QStringLiteral("50000000-0000-4000-8000-000000000021");
    transaction.expected_project_revision_id =
        QStringLiteral("50000000-0000-4000-8000-000000000022");
    transaction.operations = {
        {MapHubEditOperation::Kind::PutObject,
         object->persistentId(),
         part->persistentId(),
         {},
         1,
         MapHubEditTransaction::objectFragment(*object)},
    };
    return transaction;
  };

  auto compacted = bootstrap;
  compacted.stream_sequence = 5;
  compacted.stream_hash = QString(64, QLatin1Char('5'));
  for (auto &entity : compacted.entities) {
    if (entity.kind == QLatin1String("object") &&
        entity.id != object->persistentId()) {
      entity.version = 2;
      break;
    }
  }
  QString error;
  QVERIFY2(compacted.isValid(&error), qPrintable(error));
  QTemporaryDir operation_directory;
  QVERIFY(operation_directory.isValid());
  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", operation_directory.path().toUtf8());

  MapHubOperationStore store;
  QVERIFY2(store.open(QStringLiteral("60000000-0000-4000-8000-000000000021"),
                      &error),
           qPrintable(error));
  QVERIFY2(store.seedInitialProjection(map, &error), qPrintable(error));
  QVERIFY2(store.enqueue(make_pending(store.state(&error).client_instance_id),
                         &error),
           qPrintable(error));
  const auto old_transaction_id = store.nextPending(&error).transaction_id;
  const auto rebased_workspace_revision =
      QStringLiteral("50000000-0000-4000-8000-000000000031");
  const auto rebased_project_revision =
      QStringLiteral("50000000-0000-4000-8000-000000000032");
  QVERIFY2(store.rebasePendingOntoSnapshot(compacted,
                                           rebased_workspace_revision,
                                           rebased_project_revision, &error),
           qPrintable(error));
  const auto rebased = store.nextPending(&error);
  QVERIFY(rebased.isValid());
  QVERIFY(rebased.transaction_id != old_transaction_id);
  const auto parsed = MapHubEditTransaction::fromJson(
      QJsonDocument::fromJson(rebased.canonical_json).object(), &error);
  QCOMPARE(parsed.expected_stream_sequence, qint64(5));
  QCOMPARE(parsed.expected_stream_hash, compacted.stream_hash);
  QCOMPARE(parsed.expected_workspace_revision_id, rebased_workspace_revision);
  QCOMPARE(parsed.expected_project_revision_id, rebased_project_revision);
  QCOMPARE(parsed.operations.front().expected_version, qint64(1));

  MapHubOperationStore conflicting_store;
  QVERIFY2(conflicting_store.open(
               QStringLiteral("60000000-0000-4000-8000-000000000022"), &error),
           qPrintable(error));
  QVERIFY2(conflicting_store.seedInitialProjection(map, &error),
           qPrintable(error));
  auto conflicting_pending =
      make_pending(conflicting_store.state(&error).client_instance_id);
  QVERIFY2(conflicting_store.enqueue(conflicting_pending, &error),
           qPrintable(error));
  auto conflicting_snapshot = compacted;
  for (auto &entity : conflicting_snapshot.entities) {
    if (entity.id == object->persistentId()) {
      entity.version = 2;
      break;
    }
  }
  error.clear();
  QVERIFY(!conflicting_store.rebasePendingOntoSnapshot(
      conflicting_snapshot, rebased_workspace_revision,
      rebased_project_revision, &error));
  QVERIFY(!error.isEmpty());
  QVERIFY(conflicting_store.nextPending().isValid());

  MapHubOperationStore topology_store;
  QVERIFY2(topology_store.open(
               QStringLiteral("60000000-0000-4000-8000-000000000023"), &error),
           qPrintable(error));
  QVERIFY2(topology_store.seedInitialProjection(map, &error),
           qPrintable(error));
  auto topology_pending =
      make_pending(topology_store.state(&error).client_instance_id);
  QVERIFY2(topology_store.enqueue(topology_pending, &error), qPrintable(error));
  MapHubEditTransaction symbol_change;
  symbol_change.client_instance_id =
      QStringLiteral("40000000-0000-4000-8000-000000000031");
  symbol_change.client_sequence = 1;
  symbol_change.transaction_id =
      QStringLiteral("40000000-0000-4000-8000-000000000032");
  symbol_change.expected_stream_hash = QString(64, QLatin1Char('0'));
  symbol_change.expected_workspace_revision_id =
      topology_pending.expected_workspace_revision_id;
  symbol_change.expected_project_revision_id =
      topology_pending.expected_project_revision_id;
  const auto *symbol = map.getSymbol(0);
  symbol_change.operations = {
      {MapHubEditOperation::Kind::PutSymbol,
       symbol->persistentId(),
       {},
       {},
       1,
       MapHubEditTransaction::symbolFragment(*symbol, map)},
  };
  MapHubCommittedTransaction committed_symbol_change;
  committed_symbol_change.transaction = symbol_change;
  committed_symbol_change.stream_sequence = 1;
  committed_symbol_change.payload_sha256 = symbol_change.payloadSha256(&error);
  committed_symbol_change.stream_hash =
      MapHubOperationStore::chainHash(symbol_change.expected_stream_hash,
                                      committed_symbol_change.payload_sha256);
  committed_symbol_change.committed_at =
      QStringLiteral("2026-07-28T12:01:00+00:00");
  error.clear();
  QVERIFY(!topology_store.rebaseOnto({committed_symbol_change}, &error));
  QVERIFY(!error.isEmpty());
  QVERIFY(topology_store.nextPending().isValid());

  error.clear();
  MapHubOperationStore anchor_store;
  QVERIFY2(anchor_store.open(
               QStringLiteral("60000000-0000-4000-8000-000000000024"), &error),
           qPrintable(error));
  QVERIFY2(anchor_store.seedInitialProjection(map, &error), qPrintable(error));
  auto anchor_pending =
      make_pending(anchor_store.state(&error).client_instance_id);
  const auto *anchored_object = part->getObject(1);
  anchor_pending.operations.front().entity_id = anchored_object->persistentId();
  anchor_pending.operations.front().after_id = object->persistentId();
  anchor_pending.operations.front().payload =
      MapHubEditTransaction::objectFragment(*anchored_object);
  QVERIFY2(anchor_store.enqueue(anchor_pending, &error), qPrintable(error));
  MapHubEditTransaction anchor_change;
  anchor_change.client_instance_id =
      QStringLiteral("40000000-0000-4000-8000-000000000041");
  anchor_change.client_sequence = 1;
  anchor_change.transaction_id =
      QStringLiteral("40000000-0000-4000-8000-000000000042");
  anchor_change.expected_stream_hash = QString(64, QLatin1Char('0'));
  anchor_change.expected_workspace_revision_id =
      anchor_pending.expected_workspace_revision_id;
  anchor_change.expected_project_revision_id =
      anchor_pending.expected_project_revision_id;
  anchor_change.operations = {
      {MapHubEditOperation::Kind::PutObject,
       object->persistentId(),
       part->persistentId(),
       {},
       1,
       MapHubEditTransaction::objectFragment(*object)},
  };
  MapHubCommittedTransaction committed_anchor_change;
  committed_anchor_change.transaction = anchor_change;
  committed_anchor_change.stream_sequence = 1;
  committed_anchor_change.payload_sha256 = anchor_change.payloadSha256(&error);
  committed_anchor_change.stream_hash =
      MapHubOperationStore::chainHash(anchor_change.expected_stream_hash,
                                      committed_anchor_change.payload_sha256);
  committed_anchor_change.committed_at =
      QStringLiteral("2026-07-28T12:02:00+00:00");
  error.clear();
  QVERIFY(!anchor_store.rebaseOnto({committed_anchor_change}, &error));
  QVERIFY(!error.isEmpty());
  QVERIFY(anchor_store.nextPending().isValid());

  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
}

void MapHubProtocolFixtureTest::
    controllerBootstrapsAndJournalsStructuralEdits() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  const auto prior_workspace_root = qgetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());

  const auto map_path =
      directory.filePath(QStringLiteral("connected-field-map.omap"));
  XMLFileExporter initial_exporter(map_path, &map, nullptr);
  QVERIFY(initial_exporter.doExport());

  ManagedMapWorkspace workspace;
  workspace.local_map_path = map_path;
  workspace.server_url = QStringLiteral("https://maps.example.test");
  workspace.project_id = QStringLiteral("60000000-0000-4000-8000-000000000041");
  workspace.work_package_id =
      QStringLiteral("60000000-0000-4000-8000-000000000042");
  workspace.workspace_id =
      QStringLiteral("60000000-0000-4000-8000-000000000043");
  workspace.base_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000041");
  workspace.active_revision_id = workspace.base_revision_id;
  workspace.project_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000042");
  workspace.stream_protocol = QStringLiteral("oom-map-ops/1");
  workspace.initial_snapshot_required = true;
  workspace.stream_head_hash = QString(64, QLatin1Char('0'));
  workspace.minimum_available_sequence = 1;
  workspace.status = QStringLiteral("active");

  quint64 revision = 1;
  MapHubSyncController controller;
  controller.configure(
      workspace, &map, [&revision] { return revision; },
      [&map, &revision](const QString &destination, quint64 *saved_revision,
                        QString *error) {
        XMLFileExporter exporter(destination, &map, nullptr);
        if (!exporter.doExport()) {
          if (error)
            *error = QStringLiteral("Test export failed.");
          return false;
        }
        *saved_revision = revision;
        return true;
      });
  QVERIFY2(controller.state() != MapHubSyncController::State::ActionRequired,
           qPrintable(controller.stateText()));
  QString error;
  const auto staged = MapHubSyncQueue::load(workspace.workspace_id, &error);
  QVERIFY2(staged.isValid(), qPrintable(error));
  QVERIFY(staged.bootstrap);
  QVERIFY(staged.publish_snapshot);
  QCOMPARE(staged.base_stream_sequence, qint64(0));
  QCOMPARE(staged.base_stream_hash, QString(64, QLatin1Char('0')));

  map.getPart(0)->setName(QStringLiteral("Renamed field sheet"));
  QCoreApplication::processEvents();
  QCOMPARE(controller.pendingOperationCount(), 1);
  map.addSymbol(new PointSymbol(), map.getNumSymbols());
  QCoreApplication::processEvents();
  QCOMPARE(controller.pendingOperationCount(), 2);
  controller.clear();

  MapHubOperationStore store;
  QVERIFY2(store.open(workspace.workspace_id, &error), qPrintable(error));
  const auto pending = store.pendingTransactions(&error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(pending.size(), 2);
  QCOMPARE(pending[0].operations.size(), 1);
  QCOMPARE(pending[0].operations.front().kind,
           MapHubEditOperation::Kind::PutPart);
  QCOMPARE(pending[0].operations.front().payload,
           QStringLiteral("Renamed field sheet"));
  QCOMPARE(pending[1].operations.size(), 1);
  QCOMPARE(pending[1].operations.front().kind,
           MapHubEditOperation::Kind::PutSymbol);
  QCOMPARE(pending[1].operations.front().expected_version, qint64(0));

  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
  if (prior_workspace_root.isNull())
    qunsetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  else
    qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", prior_workspace_root);
}

void MapHubProtocolFixtureTest::controllerReplaysDurableRemoteInbox() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));
  Map sender;
  XMLFileImporter sender_importer(source, &sender, nullptr);
  QVERIFY2(sender_importer.doImport(), qPrintable(source));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  const auto prior_workspace_root = qgetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());

  ManagedMapWorkspace workspace;
  workspace.local_map_path =
      directory.filePath(QStringLiteral("remote-replay.omap"));
  workspace.server_url = QStringLiteral("https://maps.example.test");
  workspace.project_id = QStringLiteral("60000000-0000-4000-8000-000000000051");
  workspace.work_package_id =
      QStringLiteral("60000000-0000-4000-8000-000000000052");
  workspace.workspace_id =
      QStringLiteral("60000000-0000-4000-8000-000000000053");
  workspace.base_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000051");
  workspace.active_revision_id = workspace.base_revision_id;
  workspace.project_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000052");
  workspace.stream_protocol = QStringLiteral("oom-map-ops/1");
  workspace.minimum_available_sequence = 1;
  workspace.status = QStringLiteral("active");

  sender.moveSymbol(2, 0);
  const auto moved_symbol_id = sender.getSymbol(0)->persistentId();
  const auto moved_object_id = sender.getPart(0)->getObject(1)->persistentId();
  const auto object_anchor_id = sender.getPart(0)->getObject(0)->persistentId();
  MapHubEditTransaction remote;
  remote.client_instance_id =
      QStringLiteral("40000000-0000-4000-8000-000000000051");
  remote.client_sequence = 1;
  remote.transaction_id =
      QStringLiteral("40000000-0000-4000-8000-000000000052");
  remote.expected_stream_hash = QString(64, QLatin1Char('0'));
  remote.expected_workspace_revision_id = workspace.active_revision_id;
  remote.expected_project_revision_id = workspace.project_revision_id;
  remote.operations = {
      {MapHubEditOperation::Kind::PutPart,
       map.getPart(0)->persistentId(),
       {},
       {},
       1,
       QStringLiteral("Upstream field sheet")},
      {MapHubEditOperation::Kind::PutSymbol,
       moved_symbol_id,
       {},
       {},
       1,
       MapHubEditTransaction::symbolFragment(*sender.getSymbol(0), sender)},
      {MapHubEditOperation::Kind::PutObject, moved_object_id,
       sender.getPart(0)->persistentId(), object_anchor_id, 1,
       MapHubEditTransaction::objectFragment(*sender.getPart(0)->getObject(1))},
  };
  QString error;
  MapHubCommittedTransaction committed;
  committed.transaction = remote;
  committed.stream_sequence = 1;
  committed.payload_sha256 = remote.payloadSha256(&error);
  committed.stream_hash = MapHubOperationStore::chainHash(
      remote.expected_stream_hash, committed.payload_sha256);
  committed.committed_at = QStringLiteral("2026-07-28T12:02:00+00:00");

  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace.workspace_id, &error), qPrintable(error));
    QVERIFY2(store.seedInitialProjection(map, &error), qPrintable(error));
    QVERIFY2(store.rebaseOnto({committed}, &error), qPrintable(error));
    QCOMPARE(store.unappliedTransactions(&error).size(), 1);
  }
  workspace.stream_head_sequence = committed.stream_sequence;
  workspace.stream_head_hash = committed.stream_hash;

  quint64 revision = 1;
  MapHubSyncController controller;
  controller.configure(
      workspace, &map, [&revision] { return revision; },
      [&map, &revision](const QString &destination, quint64 *saved_revision,
                        QString *snapshot_error) {
        XMLFileExporter exporter(destination, &map, nullptr);
        if (!exporter.doExport()) {
          if (snapshot_error)
            *snapshot_error = QStringLiteral("Test export failed.");
          return false;
        }
        *saved_revision = revision;
        return true;
      });
  QVERIFY2(controller.state() != MapHubSyncController::State::ActionRequired,
           qPrintable(controller.stateText()));
  QCOMPARE(map.getPart(0)->getName(), QStringLiteral("Upstream field sheet"));
  QCOMPARE(map.getSymbol(0)->persistentId(), moved_symbol_id);
  QCOMPARE(map.getPart(0)->getObject(1)->persistentId(), moved_object_id);
  QCOMPARE(map.getPart(0)->getObject(1)->getSymbol()->persistentId(),
           moved_symbol_id);
  controller.clear();

  MapHubOperationStore verified;
  QVERIFY2(verified.open(workspace.workspace_id, &error), qPrintable(error));
  QVERIFY(verified.unappliedTransactions(&error).isEmpty());
  QVERIFY2(error.isEmpty(), qPrintable(error));

  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
  if (prior_workspace_root.isNull())
    qunsetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  else
    qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", prior_workspace_root);
}

void MapHubProtocolFixtureTest::controllerRestoresStickyUpstreamConflict() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  const auto prior_workspace_root = qgetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());

  const auto map_path =
      directory.filePath(QStringLiteral("sticky-upstream-conflict.omap"));
  XMLFileExporter initial_exporter(map_path, &map, nullptr);
  QVERIFY(initial_exporter.doExport());

  ManagedMapWorkspace workspace;
  workspace.local_map_path = map_path;
  workspace.server_url = QStringLiteral("https://maps.example.test");
  workspace.project_id = QStringLiteral("60000000-0000-4000-8000-000000000061");
  workspace.work_package_id =
      QStringLiteral("60000000-0000-4000-8000-000000000062");
  workspace.workspace_id =
      QStringLiteral("60000000-0000-4000-8000-000000000063");
  workspace.base_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000061");
  workspace.active_revision_id = workspace.base_revision_id;
  workspace.project_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000062");
  workspace.stream_protocol = QStringLiteral("oom-map-ops/1");
  workspace.initial_snapshot_required = true;
  workspace.stream_head_hash = QString(64, QLatin1Char('0'));
  workspace.minimum_available_sequence = 1;
  workspace.status = QStringLiteral("active");
  workspace.sync_problem = QStringLiteral("entity_conflict");

  QString error;
  QVERIFY2(ManagedMapWorkspace::save(workspace, &error), qPrintable(error));
  const auto persisted = ManagedMapWorkspace::loadForMap(map_path, &error);
  QVERIFY2(persisted.isValid(), qPrintable(error));
  QCOMPARE(persisted.sync_problem, QStringLiteral("entity_conflict"));

  quint64 revision = 1;
  int snapshot_count = 0;
  MapHubSyncController controller;
  controller.configure(
      persisted, &map, [&revision] { return revision; },
      [&map, &revision, &snapshot_count](const QString &destination,
                                         quint64 *saved_revision,
                                         QString *snapshot_error) {
        ++snapshot_count;
        XMLFileExporter exporter(destination, &map, nullptr);
        if (!exporter.doExport()) {
          if (snapshot_error)
            *snapshot_error = QStringLiteral("Test export failed.");
          return false;
        }
        *saved_revision = revision;
        return true;
      });
  QCOMPARE(controller.state(), MapHubSyncController::State::UpstreamChanged);
  QCOMPARE(controller.managedWorkspace().sync_problem,
           QStringLiteral("entity_conflict"));

  const auto snapshots_after_restore = snapshot_count;
  ++revision;
  map.getPart(0)->setName(QStringLiteral("Locally edited after conflict"));
  QCOMPARE(controller.state(), MapHubSyncController::State::UpstreamChanged);
  controller.savedExplicitly();
  QTRY_VERIFY_WITH_TIMEOUT(snapshot_count > snapshots_after_restore, 3000);
  QCOMPARE(controller.state(), MapHubSyncController::State::UpstreamChanged);
  QCOMPARE(controller.managedWorkspace().sync_problem,
           QStringLiteral("entity_conflict"));

  const auto saved = ManagedMapWorkspace::loadForMap(map_path, &error);
  QVERIFY2(saved.isValid(), qPrintable(error));
  QCOMPARE(saved.sync_problem, QStringLiteral("entity_conflict"));
  controller.clear();

  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
  if (prior_workspace_root.isNull())
    qunsetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  else
    qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", prior_workspace_root);
}

void MapHubProtocolFixtureTest::controllerRetriesRequiredWorkingCopyCommit() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  const auto prior_workspace_root = qgetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());

  ManagedMapWorkspace workspace;
  workspace.local_map_path =
      directory.filePath(QStringLiteral("provider-obligation.omap"));
  XMLFileExporter initial_exporter(workspace.local_map_path, &map, nullptr);
  QVERIFY(initial_exporter.doExport());
  workspace.server_url =
      QStringLiteral("https://provider-obligation.example.test");
  workspace.project_id = QStringLiteral("60000000-0000-4000-8000-000000000071");
  workspace.work_package_id =
      QStringLiteral("60000000-0000-4000-8000-000000000072");
  workspace.workspace_id =
      QStringLiteral("60000000-0000-4000-8000-000000000073");
  workspace.base_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000071");
  workspace.active_revision_id = workspace.base_revision_id;
  workspace.project_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000072");
  workspace.stream_protocol = QStringLiteral("oom-map-ops/1");
  workspace.minimum_available_sequence = 1;
  workspace.status = QStringLiteral("active");

  MapHubEditTransaction remote;
  remote.client_instance_id =
      QStringLiteral("40000000-0000-4000-8000-000000000071");
  remote.client_sequence = 1;
  remote.transaction_id =
      QStringLiteral("40000000-0000-4000-8000-000000000072");
  remote.expected_stream_hash = QString(64, QLatin1Char('0'));
  remote.expected_workspace_revision_id = workspace.active_revision_id;
  remote.expected_project_revision_id = workspace.project_revision_id;
  remote.operations = {
      {MapHubEditOperation::Kind::PutPart,
       map.getPart(0)->persistentId(),
       {},
       {},
       1,
       QStringLiteral("Recovered before provider write")},
  };
  QString error;
  MapHubCommittedTransaction committed;
  committed.transaction = remote;
  committed.stream_sequence = 1;
  committed.payload_sha256 = remote.payloadSha256(&error);
  committed.stream_hash = MapHubOperationStore::chainHash(
      remote.expected_stream_hash, committed.payload_sha256);
  committed.committed_at = QStringLiteral("2026-08-10T20:00:00+00:00");
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace.workspace_id, &error), qPrintable(error));
    QVERIFY2(store.seedInitialProjection(map, &error), qPrintable(error));
    QVERIFY2(store.rebaseOnto({committed}, &error), qPrintable(error));
    QCOMPARE(store.unappliedTransactions(&error).size(), 1);
  }
  workspace.stream_head_sequence = committed.stream_sequence;
  workspace.stream_head_hash = committed.stream_hash;

  quint64 revision = 1;
  bool provider_available = false;
  int commit_attempts = 0;
  QStringList attempted_paths;
  QVector<qint64> attempted_sizes;
  MapHubSyncController controller;
  QSignalSpy states(&controller, &MapHubSyncController::stateChanged);
  QVERIFY(states.isValid());
  controller.configure(
      workspace, &map, [&revision] { return revision; },
      [&map, &revision](const QString &destination, quint64 *saved_revision,
                        QString *snapshot_error) {
        XMLFileExporter exporter(destination, &map, nullptr);
        if (!exporter.doExport()) {
          if (snapshot_error)
            *snapshot_error = QStringLiteral("Test export failed.");
          return false;
        }
        *saved_revision = revision;
        return true;
      },
      [&provider_available, &commit_attempts, &attempted_paths,
       &attempted_sizes](const QString &snapshot_path, qint64 expected_size,
                         QString *commit_error) {
        ++commit_attempts;
        attempted_paths.push_back(snapshot_path);
        attempted_sizes.push_back(expected_size);
        if (provider_available)
          return true;
        if (commit_error)
          *commit_error = QStringLiteral("The document provider is busy.");
        return false;
      });

  QCOMPARE(commit_attempts, 1);
  QCOMPARE(controller.state(), MapHubSyncController::State::ActionRequired);
  QCOMPARE(controller.managedWorkspace().sync_problem,
           QStringLiteral("working_copy_commit_required"));
  const auto durable_draft =
      MapHubSyncQueue::load(workspace.workspace_id, &error);
  QVERIFY2(durable_draft.isValid(), qPrintable(error));
  QCOMPARE(QFileInfo(attempted_paths.front()).canonicalFilePath(),
           QFileInfo(durable_draft.snapshot_path).canonicalFilePath());
  QCOMPARE(attempted_sizes.front(), durable_draft.size_bytes);
  QVERIFY(QFileInfo::exists(durable_draft.snapshot_path));
  for (const auto &event : std::as_const(states)) {
    QVERIFY(event.at(0).toInt() !=
            static_cast<int>(MapHubSyncController::State::Synced));
  }
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace.workspace_id, &error), qPrintable(error));
    QCOMPARE(store.unappliedTransactions(&error).size(), 1);
  }

  provider_available = true;
  controller.retryNow();
  QTRY_COMPARE_WITH_TIMEOUT(commit_attempts, 2, 3000);
  QCOMPARE(QFileInfo(attempted_paths.back()).canonicalFilePath(),
           QFileInfo(durable_draft.snapshot_path).canonicalFilePath());
  QCOMPARE(attempted_sizes.back(), durable_draft.size_bytes);
  QTRY_VERIFY_WITH_TIMEOUT(
      controller.state() != MapHubSyncController::State::ActionRequired, 3000);
  QVERIFY(controller.managedWorkspace().sync_problem.isEmpty());
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace.workspace_id, &error), qPrintable(error));
    QVERIFY(store.unappliedTransactions(&error).isEmpty());
  }
  controller.clear();

  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
  if (prior_workspace_root.isNull())
    qunsetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  else
    qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", prior_workspace_root);
}

void MapHubProtocolFixtureTest::
    controllerDefersStructuralCapturePastRevisionIncrement() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));
  QVERIFY(map.modificationRevision() > 0);

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  const auto prior_workspace_root = qgetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());

  ManagedMapWorkspace workspace;
  workspace.local_map_path =
      directory.filePath(QStringLiteral("deferred-structure.omap"));
  XMLFileExporter initial_exporter(workspace.local_map_path, &map, nullptr);
  QVERIFY(initial_exporter.doExport());
  map.setHasUnsavedChanges(false);
  workspace.server_url = QStringLiteral("https://deferred.example.test");
  workspace.project_id = QStringLiteral("60000000-0000-4000-8000-000000000081");
  workspace.work_package_id =
      QStringLiteral("60000000-0000-4000-8000-000000000082");
  workspace.workspace_id =
      QStringLiteral("60000000-0000-4000-8000-000000000083");
  workspace.base_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000081");
  workspace.active_revision_id = workspace.base_revision_id;
  workspace.project_revision_id =
      QStringLiteral("50000000-0000-4000-8000-000000000082");
  workspace.stream_protocol = QStringLiteral("oom-map-ops/1");
  workspace.initial_snapshot_required = true;
  workspace.stream_head_hash = QString(64, QLatin1Char('0'));
  workspace.minimum_available_sequence = 1;
  workspace.status = QStringLiteral("active");

  MapHubSyncController controller;
  controller.configure(
      workspace, &map, [&map] { return map.modificationRevision(); },
      [&map](const QString &destination, quint64 *saved_revision,
             QString *snapshot_error) {
        XMLFileExporter exporter(destination, &map, nullptr);
        if (!exporter.doExport()) {
          if (snapshot_error)
            *snapshot_error = QStringLiteral("Test export failed.");
          return false;
        }
        *saved_revision = map.modificationRevision();
        return true;
      });
  QVERIFY(!controller.managedWorkspace().checkpoint_required);
  QCOMPARE(controller.pendingOperationCount(), 0);

  const auto revision_before = map.modificationRevision();
  quint64 revision_at_signal = 0;
  bool signal_seen = false;
  const auto signal_connection =
      connect(&map, &Map::symbolAdded, &map,
              [&map, &revision_at_signal, &signal_seen] {
                signal_seen = true;
                revision_at_signal = map.modificationRevision();
              });
  auto *added_symbol = new PointSymbol();
  const auto added_symbol_id = added_symbol->persistentId();
  map.addSymbol(added_symbol, map.getNumSymbols());

  QVERIFY(signal_seen);
  QCOMPARE(revision_at_signal, revision_before);
  QVERIFY(map.modificationRevision() > revision_before);
  QCOMPARE(controller.pendingOperationCount(), 0);

  QCoreApplication::processEvents();
  QCOMPARE(controller.pendingOperationCount(), 1);
  QVERIFY(!controller.managedWorkspace().checkpoint_required);

  // savedExplicitly() performs the same revision observation as the one-second
  // watcher. The deferred semantic capture must already own this revision.
  controller.savedExplicitly();
  QVERIFY(!controller.managedWorkspace().checkpoint_required);

  // Part add/move/delete uses Map's broad other-dirty bit, but the deferred
  // part operation completely represents a single-revision part change.
  const auto pending_before_part = controller.pendingOperationCount();
  auto *added_part = new MapPart(QStringLiteral("Field sheet"), &map);
  map.addPart(added_part, map.getNumParts());
  QCoreApplication::processEvents();
  QCOMPARE(controller.pendingOperationCount(), pending_before_part + 1);
  QVERIFY(!controller.managedWorkspace().checkpoint_required);

  // An unsupported map-notes revision immediately before the part signal
  // makes the revision delta larger than one, so the same part operation must
  // not hide the complete-map checkpoint obligation.
  map.setMapNotes(QStringLiteral("Unstreamed field-work context"));
  map.setOtherDirty();
  const auto pending_before_mixed_part = controller.pendingOperationCount();
  map.movePart(map.getNumParts() - 1, 0);
  QCoreApplication::processEvents();
  QCOMPARE(controller.pendingOperationCount(), pending_before_mixed_part + 1);
  QVERIFY(controller.managedWorkspace().checkpoint_required);

  // A single Change Scale command can contain both supported symbol puts and
  // non-streamed object/georeferencing changes. Streaming the symbol portion
  // must not misrepresent the complete compound revision as shared.
  const auto pending_before_scale = controller.pendingOperationCount();
  const auto scale_before = map.getScaleDenominator();
  map.changeScale(scale_before + 1000, 1.0, {0, 0}, true, true, true, false);
  QCoreApplication::processEvents();
  QVERIFY(controller.pendingOperationCount() > pending_before_scale);
  QVERIFY(controller.managedWorkspace().checkpoint_required);
  const auto expected_pending = controller.pendingOperationCount();

  disconnect(signal_connection);
  controller.clear();

  QString error;
  MapHubOperationStore store;
  QVERIFY2(store.open(workspace.workspace_id, &error), qPrintable(error));
  const auto pending = store.pendingTransactions(&error);
  QVERIFY2(error.isEmpty(), qPrintable(error));
  QCOMPARE(pending.size(), expected_pending);
  QCOMPARE(pending.front().operations.size(), 1);
  QCOMPARE(pending.front().operations.front().kind,
           MapHubEditOperation::Kind::PutSymbol);
  QCOMPARE(pending.front().operations.front().entity_id, added_symbol_id);
  QCOMPARE(pending.front().operations.front().expected_version, qint64(0));

  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
  if (prior_workspace_root.isNull())
    qunsetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  else
    qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", prior_workspace_root);
}

void MapHubProtocolFixtureTest::controllerKeepsOutboxBackoffAcrossPollSuccess() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  FixtureHttpServer server;
  if (!server.start())
    QSKIP(qPrintable(server.errorString()));

  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  const auto prior_workspace_root = qgetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());

  int sync_state_requests = 0;
  int transaction_requests = 0;
  bool return_retry_after = false;
  server.setHandler([&](const QByteArray &request, QTcpSocket *socket) {
    const auto request_line = request.left(request.indexOf("\r\n"));
    if (request_line.contains("/sync-state ")) {
      ++sync_state_requests;
      FixtureHttpServer::respond(socket, 304);
    } else if (request_line.contains("/transactions ")) {
      ++transaction_requests;
      FixtureHttpServer::respond(
          socket, return_retry_after ? 429 : 503,
          return_retry_after
              ? QByteArrayLiteral(
                    R"({"error":{"code":"rate_limited","message":"retry later"}})")
              : QByteArrayLiteral(
                    R"({"error":{"code":"network_error","message":"unexpected retry"}})"),
          return_retry_after ? QByteArrayLiteral("Retry-After: 40\r\n")
                             : QByteArray{});
    } else {
      FixtureHttpServer::respond(socket, 200, QByteArrayLiteral("{}"));
    }
  });

  const auto map_path = directory.filePath(QStringLiteral("backoff.omap"));
  XMLFileExporter initial_exporter(map_path, &map, nullptr);
  QVERIFY(initial_exporter.doExport());
  const auto workspace_id =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto workspace = fixtureWorkspace(map_path, server.url(), workspace_id);

  QString error;
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace_id, &error), qPrintable(error));
    QVERIFY2(store.seedInitialProjection(map, &error), qPrintable(error));
  }

  MapHubSyncController controller;
  QSignalSpy details(&controller, &MapHubSyncController::detailsChanged);
  QVERIFY(details.isValid());
  const auto configure = [&] {
    controller.configure(
        workspace, &map, [&map] { return map.modificationRevision(); },
        [&map](const QString &destination, quint64 *saved_revision,
               QString *snapshot_error) {
          XMLFileExporter exporter(destination, &map, nullptr);
          if (!exporter.doExport()) {
            if (snapshot_error)
              *snapshot_error = QStringLiteral("Test export failed.");
            return false;
          }
          *saved_revision = map.modificationRevision();
          return true;
        });
  };

  // Queue one valid semantic transaction without credentials, then reopen its
  // operation store to give that exact transaction a future retry deadline.
  configure();
  map.addSymbol(new PointSymbol(), map.getNumSymbols());
  QCoreApplication::processEvents();
  QCOMPARE(controller.pendingOperationCount(), 1);
  controller.clear();
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace_id, &error), qPrintable(error));
    const auto pending = store.nextPending(&error);
    QVERIFY2(pending.isValid(), qPrintable(error));
    QVERIFY2(store.recordFailure(
                 pending.client_sequence, QStringLiteral("network_error"),
                 QStringLiteral("fixture backoff"),
                 QDateTime::currentDateTimeUtc().addSecs(30), &error),
             qPrintable(error));
  }

  const auto lease_key =
      MapHubCredentials::workspaceLeaseKey(server.url(), workspace_id);
  QVERIFY2(MapHubCredentials::writeToken(server.url(),
                                         QStringLiteral("fixture-account")),
           "Could not store fixture account token");
  QVERIFY2(MapHubCredentials::writeToken(lease_key,
                                         QStringLiteral("fixture-lease")),
           "Could not store fixture lease token");

  configure();
  const auto details_before_not_modified = details.count();
  QTRY_VERIFY_WITH_TIMEOUT(sync_state_requests > 0, 3000);
  QTRY_VERIFY_WITH_TIMEOUT(details.count() > details_before_not_modified,
                           3000);
  QTest::qWait(300);
  QCOMPARE(transaction_requests, 0);
  QCOMPARE(controller.pendingOperationCount(), 1);
  const auto controller_timers = controller.findChildren<QTimer *>();
  const auto retry_deadline_survived = std::any_of(
      controller_timers.cbegin(), controller_timers.cend(), [](QTimer *timer) {
        return timer->isSingleShot() && timer->isActive() &&
               timer->interval() > 25'000 && timer->interval() <= 30'000;
      });
  QVERIFY(retry_deadline_survived);

  // An explicit user retry may bypass the old deadline, but the next 429 must
  // persist the server's bounded Retry-After rather than a local ~5s jitter.
  return_retry_after = true;
  const auto retry_requested_at = QDateTime::currentDateTimeUtc();
  controller.retryNow();
  QTRY_COMPARE_WITH_TIMEOUT(transaction_requests, 1, 3000);
  QTRY_COMPARE_WITH_TIMEOUT(controller.state(),
                            MapHubSyncController::State::WaitingForNetwork,
                            3000);
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace_id, &error), qPrintable(error));
    const auto pending = store.nextPending(&error);
    QVERIFY2(pending.isValid(), qPrintable(error));
    const auto server_delay =
        retry_requested_at.secsTo(pending.next_attempt_at);
    QVERIFY(server_delay >= 35);
    QVERIFY(server_delay <= 40);
  }

  MapHubCredentials::removeWorkspaceLease(
      server.url(), workspace_id,
      controller.managedWorkspace().client_instance_id);
  MapHubCredentials::removeToken(server.url());
  controller.clear();
  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
  if (prior_workspace_root.isNull())
    qunsetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  else
    qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", prior_workspace_root);
}

void MapHubProtocolFixtureTest::controllerCoalescesPresenceTransitions() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  FixtureHttpServer server;
  if (!server.start())
    QSKIP(qPrintable(server.errorString()));

  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  const auto prior_workspace_root = qgetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());

  QStringList presence_states;
  QPointer<QTcpSocket> first_ack_socket;
  server.setHandler([&](const QByteArray &request, QTcpSocket *socket) {
    const auto request_line = request.left(request.indexOf("\r\n"));
    if (request_line.contains("/sync-state ")) {
      FixtureHttpServer::respond(socket, 304);
      return;
    }
    if (request_line.contains("/ack ")) {
      presence_states.push_back(
          requestBody(request)
              .value(QStringLiteral("presence"))
              .toObject()
              .value(QStringLiteral("state"))
              .toString());
      if (presence_states.size() == 1) {
        first_ack_socket = socket;
        return;
      }
    }
    FixtureHttpServer::respond(socket, 200, QByteArrayLiteral("{}"));
  });

  const auto map_path = directory.filePath(QStringLiteral("presence.omap"));
  XMLFileExporter initial_exporter(map_path, &map, nullptr);
  QVERIFY(initial_exporter.doExport());
  const auto workspace_id =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto workspace = fixtureWorkspace(map_path, server.url(), workspace_id);

  QString error;
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace_id, &error), qPrintable(error));
    QVERIFY2(store.seedInitialProjection(map, &error), qPrintable(error));
  }
  const auto lease_key =
      MapHubCredentials::workspaceLeaseKey(server.url(), workspace_id);
  QVERIFY2(MapHubCredentials::writeToken(server.url(),
                                         QStringLiteral("fixture-account")),
           "Could not store fixture account token");
  QVERIFY2(MapHubCredentials::writeToken(lease_key,
                                         QStringLiteral("fixture-lease")),
           "Could not store fixture lease token");

  MapHubSyncController controller;
  controller.configure(
      workspace, &map, [&map] { return map.modificationRevision(); },
      [&map](const QString &destination, quint64 *saved_revision,
             QString *snapshot_error) {
        XMLFileExporter exporter(destination, &map, nullptr);
        if (!exporter.doExport()) {
          if (snapshot_error)
            *snapshot_error = QStringLiteral("Test export failed.");
          return false;
        }
        *saved_revision = map.modificationRevision();
        return true;
      });

  controller.applicationBecameActive();
  QTRY_COMPARE_WITH_TIMEOUT(presence_states.size(), 1, 3000);
  QCOMPARE(presence_states.front(), QStringLiteral("editing"));
  QVERIFY(first_ack_socket);

  // The away request arrives while editing is still in flight. Completing the
  // first request must immediately send the latest desired state.
  controller.applicationWillResignActive();
  FixtureHttpServer::respond(first_ack_socket, 200, QByteArrayLiteral("{}"));
  QTRY_COMPARE_WITH_TIMEOUT(presence_states.size(), 2, 3000);
  QCOMPARE(presence_states.back(), QStringLiteral("away"));

  MapHubCredentials::removeWorkspaceLease(
      server.url(), workspace_id,
      controller.managedWorkspace().client_instance_id);
  MapHubCredentials::removeToken(server.url());
  controller.clear();
  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
  if (prior_workspace_root.isNull())
    qunsetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  else
    qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", prior_workspace_root);
}

void MapHubProtocolFixtureTest::
    controllerStagesCurrentRevisionBeforeRemoteTerminalStatus() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  FixtureHttpServer server;
  if (!server.start())
    QSKIP(qPrintable(server.errorString()));

  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  const auto prior_workspace_root = qgetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());

  QPointer<QTcpSocket> sync_state_socket;
  server.setHandler([&](const QByteArray &request, QTcpSocket *socket) {
    const auto request_line = request.left(request.indexOf("\r\n"));
    if (request_line.contains("/sync-state ") && !sync_state_socket) {
      sync_state_socket = socket;
      return;
    }
    FixtureHttpServer::respond(socket, 200, QByteArrayLiteral("{}"));
  });

  const auto map_path = directory.filePath(QStringLiteral("terminal.omap"));
  XMLFileExporter initial_exporter(map_path, &map, nullptr);
  QVERIFY(initial_exporter.doExport());
  map.setHasUnsavedChanges(false);
  const auto workspace_id =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto workspace = fixtureWorkspace(map_path, server.url(), workspace_id);

  QString error;
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace_id, &error), qPrintable(error));
    QVERIFY2(store.seedInitialProjection(map, &error), qPrintable(error));
  }
  const auto lease_key =
      MapHubCredentials::workspaceLeaseKey(server.url(), workspace_id);
  QVERIFY2(MapHubCredentials::writeToken(server.url(),
                                         QStringLiteral("fixture-account")),
           "Could not store fixture account token");
  QVERIFY2(MapHubCredentials::writeToken(lease_key,
                                         QStringLiteral("fixture-lease")),
           "Could not store fixture lease token");

  MapHubSyncController controller;
  QSignalSpy details(&controller, &MapHubSyncController::detailsChanged);
  QVERIFY(details.isValid());
  QString status_during_snapshot;
  controller.configure(
      workspace, &map, [&map] { return map.modificationRevision(); },
      [&map, &controller,
       &status_during_snapshot](const QString &destination,
                                quint64 *saved_revision,
                                QString *snapshot_error) {
        status_during_snapshot = controller.managedWorkspace().status;
        XMLFileExporter exporter(destination, &map, nullptr);
        if (!exporter.doExport()) {
          if (snapshot_error)
            *snapshot_error = QStringLiteral("Test export failed.");
          return false;
        }
        *saved_revision = map.modificationRevision();
        return true;
      });
  QVERIFY(!controller.estimatedServerTime().isValid());
  QTRY_VERIFY_WITH_TIMEOUT(sync_state_socket, 3000);

  // This is deliberately a checkpoint-only edit with no structural operation
  // signal. The remote transition arrives before the normal one-second
  // observer or ten-second coalescing save can run.
  const auto note = QStringLiteral("Durable before remote submission");
  map.setMapNotes(note);
  map.setOtherDirty();
  const auto edited_revision = map.modificationRevision();
  const QJsonArray collaborators{
      QJsonObject{
          {QStringLiteral("person_id"),
           QStringLiteral("30000000-0000-4000-8000-000000000001")},
          {QStringLiteral("client_instance_id"),
           QStringLiteral("40000000-0000-4000-8000-000000000001")},
          {QStringLiteral("display_name"), QStringLiteral("Expiry fixture")},
          {QStringLiteral("last_seen_at"),
           QStringLiteral("2026-08-10T20:00:00Z")},
          {QStringLiteral("state"), QStringLiteral("editing")},
          {QStringLiteral("applied_stream_sequence"), 0},
          {QStringLiteral("is_current_user"), false},
      },
  };
  FixtureHttpServer::respond(
      sync_state_socket, 200,
      syncStateResponse(workspace, QStringLiteral("submitted"), collaborators,
                        1));

  QTRY_COMPARE_WITH_TIMEOUT(controller.managedWorkspace().status,
                            QStringLiteral("submitted"), 3000);
  const auto first_estimated_server_time = controller.estimatedServerTime();
  QVERIFY(first_estimated_server_time.isValid());
  QCOMPARE(first_estimated_server_time.date(), QDate(2026, 8, 10));
  QTest::qWait(20);
  QVERIFY(controller.estimatedServerTime() > first_estimated_server_time);
  QCOMPARE(controller.collaborators().size(), 1);
  const auto details_before_presence_expiry = details.count();
  QTRY_VERIFY_WITH_TIMEOUT(details.count() > details_before_presence_expiry,
                           1800);
  QVERIFY(controller.estimatedServerTime() >=
          QDateTime::fromString(QStringLiteral("2026-08-10T20:00:01Z"),
                                Qt::ISODate));
  QCOMPARE(status_during_snapshot, QStringLiteral("active"));
  const auto pending = MapHubSyncQueue::load(workspace_id, &error);
  QVERIFY2(pending.isValid(), qPrintable(error));
  QCOMPARE(pending.map_revision, edited_revision);
  QVERIFY(QFileInfo::exists(pending.snapshot_path));

  Map durable_working_copy;
  XMLFileImporter durable_importer(map_path, &durable_working_copy, nullptr);
  QVERIFY2(durable_importer.doImport(), qPrintable(map_path));
  QCOMPARE(durable_working_copy.getMapNotes(), note);

  MapHubCredentials::removeWorkspaceLease(
      server.url(), workspace_id,
      controller.managedWorkspace().client_instance_id);
  MapHubCredentials::removeToken(server.url());
  controller.clear();
  QVERIFY(!controller.estimatedServerTime().isValid());
  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
  if (prior_workspace_root.isNull())
    qunsetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  else
    qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", prior_workspace_root);
}

void MapHubProtocolFixtureTest::controllerKeepsWorkspaceMetadataFailureSticky() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  const auto prior_workspace_root = qgetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());

  const auto map_path = directory.filePath(QStringLiteral("metadata.omap"));
  XMLFileExporter initial_exporter(map_path, &map, nullptr);
  QVERIFY(initial_exporter.doExport());
  map.setHasUnsavedChanges(false);
  const auto workspace_id =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto workspace = fixtureWorkspace(
      map_path, QStringLiteral("https://metadata.example.test"), workspace_id);

  QString error;
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace_id, &error), qPrintable(error));
    QVERIFY2(store.seedInitialProjection(map, &error), qPrintable(error));
  }

  MapHubSyncController controller;
  controller.configure(
      workspace, &map, [&map] { return map.modificationRevision(); },
      [&map](const QString &destination, quint64 *saved_revision,
             QString *snapshot_error) {
        XMLFileExporter exporter(destination, &map, nullptr);
        if (!exporter.doExport()) {
          if (snapshot_error)
            *snapshot_error = QStringLiteral("Test export failed.");
          return false;
        }
        *saved_revision = map.modificationRevision();
        return true;
      });

  const auto blocked_root = directory.filePath(QStringLiteral("not-a-root"));
  QFile blocker(blocked_root);
  QVERIFY(blocker.open(QIODevice::WriteOnly));
  QVERIFY(blocker.write("blocked") > 0);
  blocker.close();
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", blocked_root.toUtf8());

  map.setMapNotes(QStringLiteral("Metadata must remain sticky"));
  map.setOtherDirty();
  controller.savedExplicitly();
  QCOMPARE(controller.state(), MapHubSyncController::State::ActionRequired);
  QCOMPARE(controller.managedWorkspace().sync_problem,
           QStringLiteral("workspace_metadata"));
  QVERIFY(controller.managedWorkspace().checkpoint_required);
  const auto metadata_error_text = controller.stateText();
  QVERIFY(metadata_error_text.contains(QStringLiteral("metadata"),
                                       Qt::CaseInsensitive));

  // A later checkpoint/snapshot state cannot mask the failed sidecar write.
  QCoreApplication::processEvents();
  QCOMPARE(controller.state(), MapHubSyncController::State::ActionRequired);
  QCOMPARE(controller.stateText(), metadata_error_text);
  QCOMPARE(controller.managedWorkspace().sync_problem,
           QStringLiteral("workspace_metadata"));

  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());
  controller.retryNow();
  QVERIFY(controller.managedWorkspace().sync_problem.isEmpty());
  QCOMPARE(controller.state(), MapHubSyncController::State::CheckpointNeeded);
  const auto persisted = ManagedMapWorkspace::loadForMap(map_path, &error);
  QVERIFY2(persisted.isValid(), qPrintable(error));
  QVERIFY(persisted.sync_problem.isEmpty());
  QVERIFY(persisted.checkpoint_required);

  controller.clear();
  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
  if (prior_workspace_root.isNull())
    qunsetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  else
    qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", prior_workspace_root);
}

void MapHubProtocolFixtureTest::
    controllerKeepsInboxUntilAppliedReceiptPersists() {
  const QDir test_directory(QStringLiteral(MAPPER_TEST_SOURCE_DIR));
  const auto source = test_directory.filePath(
      QStringLiteral("data/map-hub-bootstrap-source.omap"));
  Map map;
  XMLFileImporter importer(source, &map, nullptr);
  QVERIFY2(importer.doImport(), qPrintable(source));

  QTemporaryDir directory;
  QVERIFY(directory.isValid());
  const auto prior_sync_root = qgetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  const auto prior_workspace_root = qgetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());

  const auto map_path =
      directory.filePath(QStringLiteral("durable-inbox-receipt.omap"));
  XMLFileExporter initial_exporter(map_path, &map, nullptr);
  QVERIFY(initial_exporter.doExport());
  map.setHasUnsavedChanges(false);
  const auto workspace_id =
      QStringLiteral("60000000-0000-4000-8000-000000000091");
  const auto client_instance_id =
      QStringLiteral("40000000-0000-4000-8000-000000000091");
  auto workspace = fixtureWorkspace(
      map_path, QStringLiteral("https://inbox-receipt.example.test"),
      workspace_id);
  workspace.client_instance_id = client_instance_id;

  MapHubEditTransaction remote;
  remote.client_instance_id =
      QStringLiteral("40000000-0000-4000-8000-000000000092");
  remote.client_sequence = 1;
  remote.transaction_id =
      QStringLiteral("40000000-0000-4000-8000-000000000093");
  remote.expected_stream_hash = QString(64, QLatin1Char('0'));
  remote.expected_workspace_revision_id = workspace.active_revision_id;
  remote.expected_project_revision_id = workspace.project_revision_id;
  remote.operations = {
      {MapHubEditOperation::Kind::PutPart,
       map.getPart(0)->persistentId(),
       {},
       {},
       1,
       QStringLiteral("Applied receipt must persist first")},
  };
  QString error;
  MapHubCommittedTransaction committed;
  committed.transaction = remote;
  committed.stream_sequence = 1;
  committed.payload_sha256 = remote.payloadSha256(&error);
  committed.stream_hash = MapHubOperationStore::chainHash(
      remote.expected_stream_hash, committed.payload_sha256);
  committed.committed_at = QStringLiteral("2026-08-10T20:00:00+00:00");
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace_id, client_instance_id, &error),
             qPrintable(error));
    QVERIFY2(store.seedInitialProjection(map, &error), qPrintable(error));
    QVERIFY2(store.rebaseOnto({committed}, &error), qPrintable(error));
    QCOMPARE(store.unappliedTransactions(&error).size(), 1);
  }
  workspace.stream_head_sequence = committed.stream_sequence;
  workspace.stream_head_hash = committed.stream_hash;
  QVERIFY2(ManagedMapWorkspace::save(workspace, &error), qPrintable(error));

  // Make only the sidecar root unwritable. The working copy and private
  // content-addressed snapshot still commit successfully before the receipt.
  const auto blocked_root = directory.filePath(QStringLiteral("not-a-root"));
  QFile blocker(blocked_root);
  QVERIFY(blocker.open(QIODevice::WriteOnly));
  QVERIFY(blocker.write("blocked") > 0);
  blocker.close();
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", blocked_root.toUtf8());

  MapHubSyncController controller;
  controller.configure(
      workspace, &map, [&map] { return map.modificationRevision(); },
      [&map](const QString &destination, quint64 *saved_revision,
             QString *snapshot_error) {
        XMLFileExporter exporter(destination, &map, nullptr);
        if (!exporter.doExport()) {
          if (snapshot_error)
            *snapshot_error = QStringLiteral("Test export failed.");
          return false;
        }
        *saved_revision = map.modificationRevision();
        return true;
      });
  QCOMPARE(controller.state(), MapHubSyncController::State::ActionRequired);
  QCOMPARE(controller.managedWorkspace().sync_problem,
           QStringLiteral("workspace_metadata"));

  // This is the crash-safety invariant: until the applied sequence reaches
  // the sidecar, the durable inbox remains available for idempotent replay.
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace_id, client_instance_id, &error),
             qPrintable(error));
    QCOMPARE(store.unappliedTransactions(&error).size(), 1);
  }
  qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", directory.path().toUtf8());
  const auto before_retry = ManagedMapWorkspace::loadForMap(map_path, &error);
  QVERIFY2(before_retry.isValid(), qPrintable(error));
  QCOMPARE(before_retry.applied_stream_sequence, qint64(0));

  controller.retryNow();
  {
    MapHubOperationStore store;
    QVERIFY2(store.open(workspace_id, client_instance_id, &error),
             qPrintable(error));
    QVERIFY(store.unappliedTransactions(&error).isEmpty());
  }
  const auto after_retry = ManagedMapWorkspace::loadForMap(map_path, &error);
  QVERIFY2(after_retry.isValid(), qPrintable(error));
  QCOMPARE(after_retry.applied_stream_sequence, committed.stream_sequence);
  QVERIFY(after_retry.sync_problem.isEmpty());

  controller.clear();
  if (prior_sync_root.isNull())
    qunsetenv("MAPPER_MAP_HUB_SYNC_ROOT");
  else
    qputenv("MAPPER_MAP_HUB_SYNC_ROOT", prior_sync_root);
  if (prior_workspace_root.isNull())
    qunsetenv("MAPPER_MANAGED_WORKSPACE_ROOT");
  else
    qputenv("MAPPER_MANAGED_WORKSPACE_ROOT", prior_workspace_root);
}

QTEST_MAIN(MapHubProtocolFixtureTest)
