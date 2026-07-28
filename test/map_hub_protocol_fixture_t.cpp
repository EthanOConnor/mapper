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
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>
#include <QXmlStreamReader>

#include "collaboration/managed_map_workspace.h"
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
  map.addSymbol(new PointSymbol(), map.getNumSymbols());
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

QTEST_MAIN(MapHubProtocolFixtureTest)
