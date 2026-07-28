/*
 *    Copyright 2014 Kai Pastor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "undo_manager_t.h"

#include <QtTest>
#include <QTemporaryDir>

#include "collaboration/map_hub_edit_transaction.h"
#include "collaboration/map_hub_operation_store.h"
#include "core/map.h"
#include "core/map_coord.h"
#include "core/objects/object.h"
#include "core/symbols/line_symbol.h"
#include "undo/map_part_undo.h"
#include "undo/object_undo.h"
#include "undo/undo.h"
#include "undo/undo_manager.h"

namespace OpenOrienteering { class Map; }

using namespace OpenOrienteering;

void UndoManagerTest::testRejectsMalformedMapHubTransactions()
{
	MapHubEditTransaction transaction;
	transaction.client_instance_id =
	    QStringLiteral("10000000-0000-4000-8000-000000000001");
	transaction.client_sequence = 1;
	transaction.transaction_id =
	    QStringLiteral("10000000-0000-4000-8000-000000000002");
	transaction.expected_stream_hash = QString(64, QLatin1Char('0'));
	transaction.expected_workspace_revision_id =
	    QStringLiteral("10000000-0000-4000-8000-000000000003");
	transaction.operations.push_back({
	    MapHubEditOperation::Kind::PutObject,
	    QStringLiteral("20000000-0000-4000-8000-000000000001"),
	    QStringLiteral("30000000-0000-4000-8000-000000000001"),
	    {},
	    0,
	    QStringLiteral("<object uuid=\"20000000-0000-4000-8000-000000000001\"/>")
	});
	QString error;
	QVERIFY2(transaction.isValid(&error), qPrintable(error));

	auto invalid = transaction;
	invalid.operations.front().payload = QStringLiteral(
	    "<object uuid=\"20000000-0000-4000-8000-000000000002\"/>");
	QVERIFY(!invalid.isValid(&error));
	invalid = transaction;
	invalid.operations.front().payload =
	    QStringLiteral("<?xml version=\"1.0\"?><object "
	                   "uuid=\"20000000-0000-4000-8000-000000000001\"/>");
	QVERIFY(!invalid.isValid(&error));
	invalid = transaction;
	invalid.operations.front().after_id =
	    invalid.operations.front().entity_id;
	QVERIFY(!invalid.isValid(&error));
	invalid = transaction;
	invalid.expected_project_revision_id = QStringLiteral("not-a-uuid");
	QVERIFY(!invalid.isValid(&error));
}

void UndoManagerTest::testOperationStreamRebase()
{
	Map map;
	auto* symbol = new LineSymbol();
	map.addSymbol(symbol, 0);
	auto* first = new PathObject(
	    symbol, MapCoordVector{MapCoord(0, 0), MapCoord(1000, 1000)}, &map);
	auto* second = new PathObject(
	    symbol, MapCoordVector{MapCoord(2000, 0), MapCoord(3000, 1000)}, &map);
	map.addObject(first);
	map.addObject(second);

	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	qputenv("MAPPER_MAP_HUB_SYNC_ROOT", directory.path().toUtf8());
	QString error;
	MapHubOperationStore store;
	QVERIFY2(store.open(
	             QStringLiteral("00000000-0000-4000-8000-000000000020"),
	             &error),
	         qPrintable(error));
	QVERIFY2(store.seedInitialProjection(map, &error), qPrintable(error));
	const auto initial = store.state(&error);

	DeleteObjectsUndoStep local_step(&map);
	local_step.addObject(0);
	auto local = MapHubEditTransaction::fromUndoStep(
	    map, local_step, store.entityVersions(&error),
	    initial.client_instance_id, 1, 0, QString(64, QLatin1Char('0')),
	    {}, {}, &error);
	QVERIFY2(store.enqueue(local, &error), qPrintable(error));
	const auto original_pending = store.nextPending(&error);

	DeleteObjectsUndoStep upstream_step(&map);
	upstream_step.addObject(1);
	auto upstream = MapHubEditTransaction::fromUndoStep(
	    map, upstream_step, store.entityVersions(&error),
	    QStringLiteral("00000000-0000-4000-8000-000000000021"), 1, 0,
	    QString(64, QLatin1Char('0')), {}, {}, &error);
	upstream.transaction_id =
	    QStringLiteral("00000000-0000-4000-8000-000000000022");
	MapHubCommittedTransaction committed;
	committed.transaction = upstream;
	committed.stream_sequence = 1;
	committed.payload_sha256 = upstream.payloadSha256(&error);
	committed.stream_hash = MapHubOperationStore::chainHash(
	    upstream.expected_stream_hash, committed.payload_sha256);
	QVERIFY2(committed.isValid(&error), qPrintable(error));
	QVERIFY2(store.rebaseOnto({committed}, &error), qPrintable(error));

	const auto rebased = store.nextPending(&error);
	QVERIFY(rebased.isValid());
	QCOMPARE(rebased.client_sequence, qint64(1));
	QVERIFY(rebased.transaction_id != original_pending.transaction_id);
	QCOMPARE(rebased.predicted_stream_sequence, qint64(2));
	QCOMPARE(store.state(&error).published_stream_sequence, qint64(1));
	QCOMPARE(store.entityVersions(&error).value(first->persistentId()),
	         qint64(2));
	QCOMPARE(store.entityVersions(&error).value(second->persistentId()),
	         qint64(2));
}

void UndoManagerTest::testMapPartIdentitySurvivesUndoRedo()
{
	Map map;
	auto* part = new MapPart(QStringLiteral("Field sheet"), &map);
	const auto persistent_id = part->persistentId();
	map.addPart(part, 1);
	map.push(new MapPartUndoStep(&map, MapPartUndoStep::RemoveMapPart, part));
	QVERIFY(map.undoManager().undo());
	QCOMPARE(map.getNumParts(), 1);
	QVERIFY(map.undoManager().redo());
	QCOMPARE(map.getNumParts(), 2);
	QCOMPARE(map.getPart(1)->persistentId(), persistent_id);
}

void UndoManagerTest::testExternalObjectIndexAdjustment()
{
	Map map;
	auto* symbol = new LineSymbol();
	map.addSymbol(symbol, 0);
	auto* first = new PathObject(
	    symbol, MapCoordVector{MapCoord(0, 0), MapCoord(1000, 1000)}, &map);
	auto* second = new PathObject(
	    symbol, MapCoordVector{MapCoord(2000, 0), MapCoord(3000, 1000)}, &map);
	map.addObject(first);
	map.addObject(second);
	const auto second_id = second->persistentId();
	auto step = std::make_unique<DeleteObjectsUndoStep>(&map);
	step->addObject(1);
	map.undoManager().push(std::move(step));

	map.undoManager().adjustForExternalObjectChange(
	    0, 0, 1, QStringLiteral("00000000-0000-4000-8000-000000000030"));
	auto* inserted = new PathObject(
	    symbol, MapCoordVector{MapCoord(-2000, 0), MapCoord(-1000, 1000)},
	    &map);
	map.getPart(0)->addObject(inserted, 0);
	std::vector<UndoStep::EntityChange> changes;
	map.undoManager().nextUndoStep()->collectEntityChanges(changes);
	QCOMPARE(changes.size(), std::size_t(1));
	QCOMPARE(changes.front().object_id, second_id);

	map.undoManager().adjustForExternalObjectChange(0, 2, 0, second_id);
	QVERIFY(!map.undoManager().canUndo());
}

void UndoManagerTest::testCommittedEntityChanges()
{
	Map map;
	auto* symbol = new LineSymbol();
	map.addSymbol(symbol, 0);
	auto* object = new PathObject(
	    symbol, MapCoordVector{MapCoord(0, 0), MapCoord(1000, 1000)}, &map);
	const auto object_id = object->persistentId();
	const auto object_index = map.addObject(object);

	std::vector<UndoStep::EntityChange> latest;
	int emission_count = 0;
	connect(&map.undoManager(), &UndoManager::editCommitted, this,
	        [&](const UndoStep* step) {
		        latest.clear();
		        step->collectEntityChanges(latest);
		        ++emission_count;
	        });

	auto create_step = std::make_unique<DeleteObjectsUndoStep>(&map);
	create_step->addObject(object_index);
	map.undoManager().push(std::move(create_step));
	QCOMPARE(emission_count, 1);
	QCOMPARE(latest.size(), std::size_t(1));
	QCOMPARE(latest.front().type, UndoStep::EntityChangeType::PutObject);
	QCOMPARE(latest.front().object_id, object_id);
	QCOMPARE(latest.front().object, object);

	QString transaction_error;
	auto put_transaction = MapHubEditTransaction::fromUndoStep(
	    map, *map.undoManager().nextUndoStep(), {},
	    QStringLiteral("00000000-0000-4000-8000-000000000001"), 1, 0,
	    QString(64, QLatin1Char('0')), {}, {}, &transaction_error);
	QVERIFY2(put_transaction.isValid(), qPrintable(transaction_error));
	QCOMPARE(put_transaction.operations.size(), 1);
	QCOMPARE(put_transaction.operations.front().kind,
	         MapHubEditOperation::Kind::PutObject);
	QCOMPARE(put_transaction.operations.front().entity_id, object_id);
	QCOMPARE(put_transaction.operations.front().parent_id,
	         map.getPart(0)->persistentId());
	QVERIFY(put_transaction.operations.front().payload.startsWith(
	    QStringLiteral("<object ")));
	QVERIFY(put_transaction.operations.front().payload.contains(
	    QStringLiteral("uuid=\"%1\"").arg(object_id)));
	QVERIFY(!put_transaction.payloadSha256(&transaction_error).isEmpty());

	QTemporaryDir operation_directory;
	QVERIFY(operation_directory.isValid());
	qputenv("MAPPER_MAP_HUB_SYNC_ROOT",
	        operation_directory.path().toUtf8());
	MapHubOperationStore store;
	QVERIFY2(store.open(
	             QStringLiteral("00000000-0000-4000-8000-000000000010"),
	             &transaction_error),
	         qPrintable(transaction_error));
	QVERIFY2(store.seedInitialProjection(map, &transaction_error),
	         qPrintable(transaction_error));
	const auto store_state = store.state(&transaction_error);
	auto durable_transaction = MapHubEditTransaction::fromUndoStep(
	    map, *map.undoManager().nextUndoStep(),
	    store.entityVersions(&transaction_error),
	    store_state.client_instance_id, 1,
	    store_state.optimistic_stream_sequence,
	    store_state.optimistic_stream_hash, {}, {}, &transaction_error);
	QVERIFY2(store.enqueue(durable_transaction, &transaction_error),
	         qPrintable(transaction_error));
	const auto pending = store.nextPending(&transaction_error);
	QVERIFY(pending.isValid());
	QCOMPARE(pending.client_sequence, 1);
	QCOMPARE(pending.canonical_json,
	         durable_transaction.canonicalBytes(&transaction_error));
	QCOMPARE(pending.predicted_stream_hash,
	         MapHubOperationStore::chainHash(
	             durable_transaction.expected_stream_hash,
	             durable_transaction.payloadSha256(&transaction_error)));
	QVERIFY2(store.acknowledge(
	             pending.client_sequence, pending.predicted_stream_sequence,
	             pending.predicted_stream_hash, &transaction_error),
	         qPrintable(transaction_error));
	QCOMPARE(store.pendingCount(&transaction_error), 0);
	QCOMPARE(store.state(&transaction_error).acknowledged_client_sequence, 1);

	QVERIFY(map.undoManager().undo());
	QCOMPARE(emission_count, 2);
	QCOMPARE(latest.size(), std::size_t(1));
	QCOMPARE(latest.front().type, UndoStep::EntityChangeType::DeleteObject);
	QCOMPARE(latest.front().object_id, object_id);
	QCOMPARE(map.getNumObjects(), 0);

	auto delete_transaction = MapHubEditTransaction::fromUndoStep(
	    map, *map.undoManager().nextRedoStep(), {{object_id, 1}},
	    QStringLiteral("00000000-0000-4000-8000-000000000001"), 2, 1,
	    QString(64, QLatin1Char('1')), {}, {}, &transaction_error);
	QVERIFY2(delete_transaction.isValid(), qPrintable(transaction_error));
	QCOMPARE(delete_transaction.operations.size(), 1);
	QCOMPARE(delete_transaction.operations.front().kind,
	         MapHubEditOperation::Kind::DeleteObject);
	QCOMPARE(delete_transaction.operations.front().expected_version, 1);

	QVERIFY(map.undoManager().redo());
	QCOMPARE(emission_count, 3);
	QCOMPARE(latest.size(), std::size_t(1));
	QCOMPARE(latest.front().type, UndoStep::EntityChangeType::PutObject);
	QCOMPARE(latest.front().object_id, object_id);
	QCOMPARE(map.getNumObjects(), 1);
}


// test
void UndoManagerTest::testUndoRedo()
{
	Map* const map = nullptr;
	UndoManager undo_manager(map);
	connect(&undo_manager, &UndoManager::loadedChanged,  this, &UndoManagerTest::loadedChanged);
	connect(&undo_manager, &UndoManager::cleanChanged,   this, &UndoManagerTest::cleanChanged);
	connect(&undo_manager, &UndoManager::canRedoChanged, this, &UndoManagerTest::canRedoChanged);
	connect(&undo_manager, &UndoManager::canUndoChanged, this, &UndoManagerTest::canUndoChanged);
	
	undo_manager.setClean();
	QVERIFY(undo_manager.isClean());
	QVERIFY(!undo_manager.canRedo());
	QVERIFY(!undo_manager.canUndo());
	
	undo_manager.setLoaded();
	QVERIFY(undo_manager.isLoaded());
	
	// State: [loaded,clean,current]:EOL
	
	// Add undo step A
	resetAllChanged();
	undo_manager.push(std::unique_ptr<UndoStep>(new NoOpUndoStep(map, true)));
	
	// State: [current, loaded]:A, [clean]:EOL
	QVERIFY(loaded_changed);
	QVERIFY(!loaded);
	QVERIFY(!undo_manager.isLoaded());
	QVERIFY(clean_changed);
	QVERIFY(!clean);
	QVERIFY(!undo_manager.isClean());
	QVERIFY(can_undo_changed);
	QVERIFY(can_undo);
	QVERIFY(undo_manager.canUndo());
	QVERIFY(!can_redo_changed);
	QVERIFY(!undo_manager.canRedo());
	
	// Add undo step B
	resetAllChanged();
	undo_manager.push(std::unique_ptr<UndoStep>(new NoOpUndoStep(map, true)));
	
	// State: [loaded,clean]:A, B, [current]:EOL
	QVERIFY(!loaded_changed);
	QVERIFY(!undo_manager.isLoaded());
	QVERIFY(!clean_changed);
	QVERIFY(!undo_manager.isClean());
	QVERIFY(!can_undo_changed);
	QVERIFY(undo_manager.canUndo());
	QVERIFY(!can_redo_changed);
	QVERIFY(!undo_manager.canRedo());
	
	// Perform undo
	resetAllChanged();
	QVERIFY(undo_manager.undo());
	
	// State: [loaded,clean]:A, [current]:B, EOL
	QVERIFY(!loaded_changed);
	QVERIFY(!undo_manager.isLoaded());
	QVERIFY(!clean_changed);
	QVERIFY(!undo_manager.isClean());
	QVERIFY(!can_undo_changed);
	QVERIFY(undo_manager.canUndo());
	QVERIFY(can_redo_changed);
	QVERIFY(can_redo);
	QVERIFY(undo_manager.canRedo());
	
	// Perform another undo
	resetAllChanged();
	QVERIFY(undo_manager.undo());
	
	// State: [loaded,clean,current]:A, B, EOL
	QVERIFY(loaded_changed);
	QVERIFY(loaded);
	QVERIFY(undo_manager.isLoaded());
	QVERIFY(clean_changed);
	QVERIFY(clean);
	QVERIFY(undo_manager.isClean());
	QVERIFY(can_undo_changed);
	QVERIFY(!can_undo);
	QVERIFY(!undo_manager.canUndo());
	QVERIFY(!can_redo_changed);
	QVERIFY(undo_manager.canRedo());
	
	// Perform redo
	resetAllChanged();
	QVERIFY(undo_manager.redo());
	
	// State: [loaded,clean]:A, [current]:B, EOL
	QVERIFY(loaded_changed);
	QVERIFY(!loaded);
	QVERIFY(!undo_manager.isLoaded());
	QVERIFY(clean_changed);
	QVERIFY(!clean);
	QVERIFY(!undo_manager.isClean());
	QVERIFY(can_undo_changed);
	QVERIFY(can_undo);
	QVERIFY(undo_manager.canUndo());
	QVERIFY(!can_redo_changed);
	QVERIFY(undo_manager.canRedo());
	
	// Mark as clean
	resetAllChanged();
	undo_manager.setClean();
	
	// State: [loaded]:A, [clean,current]:B, EOL
	QVERIFY(clean_changed);
	QVERIFY(clean);
	QVERIFY(undo_manager.isClean());
	QVERIFY(!can_undo_changed);
	QVERIFY(!can_redo_changed);
	
	// Add undo step C
	resetAllChanged();
	undo_manager.push(std::unique_ptr<UndoStep>(new NoOpUndoStep(map, true)));
	
	// State: [loaded]:A, [clean]:B, [current]:C, EOL
	QVERIFY(!loaded_changed);
	QVERIFY(!undo_manager.isLoaded());
	QVERIFY(clean_changed);
	QVERIFY(!clean);
	QVERIFY(!undo_manager.isClean());
	QVERIFY(!can_undo_changed);
	QVERIFY(undo_manager.canUndo());
	QVERIFY(can_redo_changed);
	QVERIFY(!can_redo);
	QVERIFY(!undo_manager.canRedo());
	
	// Perform undo
	resetAllChanged();
	QVERIFY(undo_manager.undo());
	
	// State: [loaded]:A, [clean,current]:B, C, EOL
	QVERIFY(!loaded_changed);
	QVERIFY(!undo_manager.isLoaded());
	QVERIFY(clean_changed);
	QVERIFY(clean);
	QVERIFY(undo_manager.isClean());
	QVERIFY(!can_undo_changed);
	QVERIFY(undo_manager.canUndo());
	QVERIFY(can_redo_changed);
	QVERIFY(can_redo);
	QVERIFY(undo_manager.canRedo());
	
	// Perform another undo
	resetAllChanged();
	QVERIFY(undo_manager.undo());
	
	// State: [loaded,current]:A, [clean]:B, C, EOL
	QVERIFY(loaded_changed);
	QVERIFY(loaded);
	QVERIFY(undo_manager.isLoaded());
	QVERIFY(clean_changed);
	QVERIFY(!clean);
	QVERIFY(!undo_manager.isClean());
	QVERIFY(can_undo_changed);
	QVERIFY(!can_undo);
	QVERIFY(!undo_manager.canUndo());
	QVERIFY(!can_redo_changed);
	QVERIFY(undo_manager.canRedo());
	
	// Add undo step D: clean state now no longer reachable
	resetAllChanged();
	undo_manager.push(std::unique_ptr<UndoStep>(new NoOpUndoStep(map, true)));
	
	// State: [loaded,current]:D, EOL [clean:invalid]
	QVERIFY(loaded_changed);
	QVERIFY(!loaded);
	QVERIFY(!undo_manager.isLoaded());
	QVERIFY(!clean_changed);
	QVERIFY(!undo_manager.isClean());
	QVERIFY(can_undo_changed);
	QVERIFY(can_undo);
	QVERIFY(undo_manager.canUndo());
	QVERIFY(can_redo_changed);
	QVERIFY(!can_redo);
	QVERIFY(!undo_manager.canRedo());
}

void UndoManagerTest::resetAllChanged()
{
	loaded_changed   = false;
	clean_changed    = false;
	can_undo_changed = false;
	can_redo_changed = false;
}

// slot
void UndoManagerTest::loadedChanged(bool loaded)
{
	this->loaded = loaded;
	this->loaded_changed = true;
}

// slot
void UndoManagerTest::cleanChanged(bool clean)
{
	this->clean = clean;
	this->clean_changed = true;
}

// slot
void UndoManagerTest::canUndoChanged(bool can_undo)
{
	this->can_undo = can_undo;
	this->can_undo_changed = true;
}

// slot
void UndoManagerTest::canRedoChanged(bool can_redo)
{
	this->can_redo = can_redo;
	this->can_redo_changed = true;
}


namespace {
	[[maybe_unused]] const auto qpa_selected =
	    qputenv("QT_QPA_PLATFORM", "offscreen");
}

QTEST_MAIN(UndoManagerTest)
