/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_protocol_fixture_t.h"

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QHash>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include "collaboration/map_hub_edit_transaction.h"
#include "collaboration/map_hub_entity_index.h"
#include "collaboration/map_hub_operation_store.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "core/symbols/symbol.h"
#include "fileformats/xml_file_format_p.h"
#include "test_config.h"
#include "undo/object_undo.h"
#include "undo/undo.h"

using namespace OpenOrienteering;

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

QTEST_MAIN(MapHubProtocolFixtureTest)
