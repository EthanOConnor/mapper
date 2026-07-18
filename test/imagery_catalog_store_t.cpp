/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "imagery_catalog_store_t.h"

#include <functional>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "imagery/imagery_catalog_store.h"

using namespace OpenOrienteering;

namespace {

QJsonObject minimalSource()
{
	return {
		{ QStringLiteral("id"), QStringLiteral("aerial") },
		{ QStringLiteral("name"), QStringLiteral("Example aerial") },
		{ QStringLiteral("type"), QStringLiteral("raster-tiles") },
		{ QStringLiteral("tiles"), QJsonArray {
			QStringLiteral(
				"https://tiles.example.test/aerial/{z}/{x}/{y}.png"),
		} },
		{ QStringLiteral("scheme"), QStringLiteral("xyz") },
		{ QStringLiteral("tileMatrixSetURI"),
		  QStringLiteral(
			  "http://www.opengis.net/def/tilematrixset/OGC/1.0/"
			  "WebMercatorQuad") },
	};
}

QJsonObject minimalCatalog()
{
	return {
		{ QStringLiteral("format"),
		  QStringLiteral("org.openorienteering.imagery-catalog") },
		{ QStringLiteral("version"), 1 },
		{ QStringLiteral("id"),
		  QStringLiteral("org.example.imagery.minimal") },
		{ QStringLiteral("revision"), 1 },
		{ QStringLiteral("name"),
		  QStringLiteral("Minimal catalog") },
		{ QStringLiteral("sources"),
		  QJsonArray { minimalSource() } },
	};
}

QByteArray compact(const QJsonObject& object)
{
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

imagery::OicCatalogReadResult catalog(
	const std::function<void(QJsonObject&)>& change = {})
{
	auto object = minimalCatalog();
	if (change)
		change(object);
	return imagery::OicCatalogReader::read(compact(object));
}

QString currentPath(
	const imagery::ImageryCatalogStore& store,
	const QString& catalog_id)
{
	return QDir(
		QDir(store.rootPath()).filePath(
			store.directoryKey(catalog_id)))
		.filePath(QStringLiteral("current.json"));
}

}  // namespace


void ImageryCatalogStoreTest::installsAndLoadsAtomicSnapshot()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::ImageryCatalogStore store(
		directory.filePath(QStringLiteral("catalogs")));
	auto const candidate = catalog();
	QVERIFY(candidate.accepted());

	QString error;
	QVERIFY2(
		store.install(
			candidate,
			QStringLiteral("https://example.test/catalog.oic"),
			QByteArray("etag-one"),
			QByteArray("yesterday"),
			&error),
		qPrintable(error));

	QVector<imagery::ImageryCatalogStoreIssue> issues;
	auto const installed = store.catalogs(&issues);
	QCOMPARE(issues.size(), 0);
	QCOMPARE(installed.size(), 1);
	auto const& first = installed.first();
	QCOMPARE(
		first.read_result.catalog.original_bytes,
		candidate.catalog.original_bytes);
	QCOMPARE(
		first.state.origin,
		QStringLiteral("https://example.test/catalog.oic"));
	QCOMPARE(first.state.etag, QByteArray("etag-one"));
	QCOMPARE(first.state.last_modified, QByteArray("yesterday"));
	QVERIFY(first.state.installed_at.isValid());
	QVERIFY(first.state.updated_at.isValid());
	QVERIFY(first.state.checked_at.isValid());
	QVERIFY(!first.state.legacy_layout);
	QCOMPARE(first.state.sha256, candidate.catalog.document_sha256);
	QVERIFY(QFileInfo::exists(first.catalog_path));
	QVERIFY(QFileInfo::exists(currentPath(store, candidate.catalog.id)));
	QVERIFY(!store.directoryKey(
		QStringLiteral("../../unsafe catalog")).contains(QLatin1Char('/')));
}


void ImageryCatalogStoreTest::analyzesUpdatesAndDuplicates()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::ImageryCatalogStore store(
		directory.filePath(QStringLiteral("catalogs")));
	auto const original = catalog();
	QString error;
	QVERIFY(store.install(
		original, QStringLiteral("local"), {}, {}, &error));

	auto exact = store.analyze(original);
	QCOMPARE(
		exact.update_kind,
		imagery::ImageryCatalogAnalysis::UpdateKind::ExactReimport);
	QCOMPARE(exact.added, 0);
	QCOMPARE(exact.changed, 0);
	QCOMPARE(exact.removed, 0);

	auto const higher = catalog([](QJsonObject& object) {
		object.insert(QStringLiteral("revision"), 2);
		auto sources =
			object.value(QStringLiteral("sources")).toArray();
		auto source = sources.first().toObject();
		source.insert(
			QStringLiteral("name"),
			QStringLiteral("Updated aerial"));
		sources.replace(0, source);
		sources.push_back(QJsonObject {
			{ QStringLiteral("id"), QStringLiteral("map") },
			{ QStringLiteral("name"), QStringLiteral("Map") },
			{ QStringLiteral("type"), QStringLiteral("raster-tiles") },
			{ QStringLiteral("tiles"), QJsonArray {
				QStringLiteral(
					"https://tiles.example.test/map/{z}/{x}/{y}.png"),
			} },
			{ QStringLiteral("scheme"), QStringLiteral("xyz") },
			{ QStringLiteral("tileMatrixSetURI"),
			  QStringLiteral(
				  "http://www.opengis.net/def/tilematrixset/OGC/1.0/"
				  "WebMercatorQuad") },
		});
		object.insert(QStringLiteral("sources"), sources);
	});
	auto const higher_analysis = store.analyze(higher);
	QCOMPARE(
		higher_analysis.update_kind,
		imagery::ImageryCatalogAnalysis::UpdateKind::HigherRevision);
	QCOMPARE(higher_analysis.added, 1);
	QCOMPARE(higher_analysis.changed, 1);
	QCOMPARE(higher_analysis.operational_changed, 0);
	QCOMPARE(higher_analysis.metadata_only_changed, 1);
	QCOMPARE(higher_analysis.source_changes.size(), 2);

	auto const conflict = catalog([](QJsonObject& object) {
		object.insert(
			QStringLiteral("name"),
			QStringLiteral("Republished catalog"));
	});
	QCOMPARE(
		store.analyze(conflict).update_kind,
		imagery::ImageryCatalogAnalysis::UpdateKind::
			SameRevisionConflict);

	auto duplicate = catalog([](QJsonObject& object) {
		object.insert(
			QStringLiteral("id"),
			QStringLiteral("org.example.imagery.duplicate"));
	});
	QCOMPARE(store.analyze(duplicate).exact_duplicates, 1);
	QVERIFY(store.install(
		duplicate, QStringLiteral("local"), {}, {}, &error));

	auto potential = catalog([](QJsonObject& object) {
		object.insert(
			QStringLiteral("id"),
			QStringLiteral("org.example.imagery.potential"));
		auto sources =
			object.value(QStringLiteral("sources")).toArray();
		auto source = sources.first().toObject();
		source.insert(
			QStringLiteral("name"),
			QStringLiteral("Different description"));
		sources.replace(0, source);
		object.insert(QStringLiteral("sources"), sources);
	});
	QCOMPARE(store.analyze(potential).potential_duplicates, 1);
}


void ImageryCatalogStoreTest::enforcesRevisionPolicy()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::ImageryCatalogStore store(
		directory.filePath(QStringLiteral("catalogs")));
	auto const original = catalog();
	auto const conflict = catalog([](QJsonObject& object) {
		object.insert(
			QStringLiteral("name"),
			QStringLiteral("Republished catalog"));
	});
	auto const higher = catalog([](QJsonObject& object) {
		object.insert(QStringLiteral("revision"), 2);
	});
	QString error;
	QVERIFY(store.install(
		original, QStringLiteral("local"), {}, {}, &error));
	QVERIFY(!store.install(
		conflict, QStringLiteral("local"), {}, {}, &error));
	QVERIFY(error.contains(QStringLiteral("republished")));

	imagery::ImageryCatalogInstallOptions conflict_options;
	conflict_options.allow_same_revision_conflict = true;
	QVERIFY(store.install(
		conflict,
		imagery::ImageryCatalogInstallMetadata {
			QStringLiteral("local"),
			QStringLiteral("local"),
			{},
			{},
		},
		conflict_options,
		&error));
	QVERIFY(store.install(
		higher, QStringLiteral("local"), {}, {}, &error));
	QVERIFY(!store.install(
		original, QStringLiteral("local"), {}, {}, &error));
	QVERIFY(error.contains(QStringLiteral("older")));

	imagery::ImageryCatalogInstallOptions lower_options;
	lower_options.allow_lower_revision = true;
	QVERIFY(store.install(
		original,
		imagery::ImageryCatalogInstallMetadata {
			QStringLiteral("local"),
			QStringLiteral("local"),
			{},
			{},
		},
		lower_options,
		&error));
	QCOMPARE(
		store.catalogs().first().read_result.catalog.revision,
		1);
}


void ImageryCatalogStoreTest::
	preservesPreviousSnapshotAndMarksChecks()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::ImageryCatalogStore store(
		directory.filePath(QStringLiteral("catalogs")));
	auto const first = catalog();
	auto const second = catalog([](QJsonObject& object) {
		object.insert(QStringLiteral("revision"), 2);
	});
	QString error;
	QVERIFY(store.install(
		first, QStringLiteral("https://example.test/catalog.oic"),
		QByteArray("one"), QByteArray("old"), &error));
	auto const installed_at =
		store.catalogs().first().state.installed_at;
	QVERIFY(store.install(
		second, QStringLiteral("https://example.test/catalog.oic"),
		QByteArray("two"), QByteArray("new"), &error));

	auto installed = store.catalogs();
	QCOMPARE(installed.size(), 1);
	QCOMPARE(installed.first().state.installed_at, installed_at);
	QCOMPARE(
		installed.first().state.previous_sha256,
		first.catalog.document_sha256);
	auto const previous_path = QDir(
		installed.first().directory)
		.filePath(
			QStringLiteral("snapshots/")
			+ QString::fromLatin1(first.catalog.document_sha256)
			+ QStringLiteral("/catalog.oic"));
	QVERIFY(QFileInfo::exists(previous_path));

	auto const checked_before =
		installed.first().state.checked_at;
	QTest::qSleep(2);
	QVERIFY(store.markChecked(
		second.catalog.id,
		QStringLiteral("https://cdn.example.test/catalog.oic"),
		QByteArray("three"),
		QByteArray("newer"),
		&error));
	installed = store.catalogs();
	QCOMPARE(installed.first().state.etag, QByteArray("three"));
	QCOMPARE(
		installed.first().state.final_url,
		QStringLiteral("https://cdn.example.test/catalog.oic"));
	QCOMPARE(
		installed.first().state.last_modified,
		QByteArray("newer"));
	QVERIFY(
		installed.first().state.checked_at >= checked_before);
}


void ImageryCatalogStoreTest::
	preservesRemoteProvenanceAndSanitizesValidators()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::ImageryCatalogStore store(
		directory.filePath(QStringLiteral("catalogs")));
	auto const candidate = catalog();
	QString error;
	QVERIFY(store.install(
		candidate,
		imagery::ImageryCatalogInstallMetadata {
			QStringLiteral("https://publisher.example/catalog.oic"),
			QStringLiteral("https://cdn.example/catalog.oic"),
			QByteArray("\"remote-etag\""),
			QByteArray("Thu, 16 Jul 2026 12:00:00 GMT"),
		},
		{},
		&error));

	QVERIFY(store.install(
		candidate,
		imagery::ImageryCatalogInstallMetadata {
			QStringLiteral("file:///tmp/catalog.oic"),
			QStringLiteral("file:///tmp/catalog.oic"),
			QByteArray("local-etag"),
			QByteArray("local-date"),
		},
		{},
		&error));
	auto installed = store.catalogs();
	QCOMPARE(installed.size(), 1);
	QCOMPARE(
		installed.first().state.origin,
		QStringLiteral("https://publisher.example/catalog.oic"));
	QCOMPARE(
		installed.first().state.final_url,
		QStringLiteral("https://cdn.example/catalog.oic"));
	QCOMPARE(
		installed.first().state.etag,
		QByteArray("\"remote-etag\""));
	QCOMPARE(
		installed.first().state.last_modified,
		QByteArray("Thu, 16 Jul 2026 12:00:00 GMT"));

	QVERIFY(store.markChecked(
		candidate.catalog.id,
		QStringLiteral("https://cdn.example/catalog.oic"),
		QByteArray("unsafe\r\nInjected: true"),
		QByteArray(5000, 'x'),
		&error));
	installed = store.catalogs();
	QCOMPARE(installed.size(), 1);
	QVERIFY(installed.first().state.etag.isEmpty());
	QVERIFY(installed.first().state.last_modified.isEmpty());
}


void ImageryCatalogStoreTest::recoversPreviousSnapshot()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::ImageryCatalogStore store(
		directory.filePath(QStringLiteral("catalogs")));
	auto const first = catalog();
	auto const second = catalog([](QJsonObject& object) {
		object.insert(QStringLiteral("revision"), 2);
	});
	QString error;
	QVERIFY(store.install(
		first,
		imagery::ImageryCatalogInstallMetadata {
			QStringLiteral("https://publisher.example/catalog.oic"),
			QStringLiteral("https://cdn-v1.example/catalog.oic"),
			QByteArray("etag-v1"),
			QByteArray("date-v1"),
		},
			{},
			&error));
	auto const first_updated_at =
		store.catalogs()
			.first().state.updated_at;
	QVERIFY(store.install(
		first,
		imagery::ImageryCatalogInstallMetadata {
			QStringLiteral("https://publisher-new.example/catalog.oic"),
			QStringLiteral("https://cdn-new.example/catalog.oic"),
			QByteArray("etag-reimport"),
			QByteArray("date-reimport"),
		},
		{},
		&error));
	QVERIFY(store.markChecked(
		first.catalog.id,
		QStringLiteral("https://cdn-validated.example/catalog.oic"),
		QByteArray("etag-validated"),
		QByteArray("date-validated"),
		&error));
	auto refreshed = store.catalogs();
	QCOMPARE(refreshed.size(), 1);
	QCOMPARE(
		refreshed.first().state.origin,
		QStringLiteral("https://publisher-new.example/catalog.oic"));
	QCOMPARE(
		refreshed.first().state.final_url,
		QStringLiteral("https://cdn-validated.example/catalog.oic"));
	QCOMPARE(
		refreshed.first().state.updated_at,
		first_updated_at);
	QVERIFY(store.install(
		second,
		imagery::ImageryCatalogInstallMetadata {
			QStringLiteral("file:///tmp/catalog-v2.oic"),
			QStringLiteral("file:///tmp/catalog-v2.oic"),
			QByteArray("etag-v2"),
			QByteArray("date-v2"),
		},
		{},
		&error));
	auto installed = store.catalogs();
	QCOMPARE(installed.size(), 1);
	QFile active(installed.first().catalog_path);
	QVERIFY(active.open(QIODevice::WriteOnly | QIODevice::Truncate));
	active.write("damaged");
	active.close();

	QVector<imagery::ImageryCatalogStoreIssue> issues;
	installed = store.catalogs(&issues);
	QCOMPARE(installed.size(), 1);
	QCOMPARE(issues.size(), 1);
	QVERIFY(installed.first().state.recovered_previous);
	QCOMPARE(
		installed.first().read_result.catalog.revision,
		1);
	QCOMPARE(
		installed.first().state.sha256,
		first.catalog.document_sha256);
	QCOMPARE(
		installed.first().state.final_url,
		QStringLiteral("https://cdn-validated.example/catalog.oic"));
	QCOMPARE(
		installed.first().state.origin,
		QStringLiteral("https://publisher-new.example/catalog.oic"));
	QCOMPARE(
		installed.first().state.updated_at,
		first_updated_at);
	QCOMPARE(
		installed.first().state.etag,
		QByteArray("etag-validated"));
	QCOMPARE(
		installed.first().state.last_modified,
		QByteArray("date-validated"));

	QVERIFY(store.markChecked(
		first.catalog.id,
		QStringLiteral("https://publisher.example/catalog.oic"),
		{},
		{},
		&error));
	issues.clear();
	installed = store.catalogs(&issues);
	QCOMPARE(installed.size(), 1);
	QCOMPARE(issues.size(), 0);
	QVERIFY(!installed.first().state.recovered_previous);
	QCOMPARE(
		installed.first().read_result.catalog.revision,
		1);
}


void ImageryCatalogStoreTest::isolatesCorruptEntries()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::ImageryCatalogStore store(
		directory.filePath(QStringLiteral("catalogs")));
	auto const valid = catalog();
	QString error;
	QVERIFY(store.install(
		valid, QStringLiteral("local"), {}, {}, &error));

	auto const corrupt =
		QDir(store.rootPath()).filePath(
			QStringLiteral("00000000000000000000000000000000"));
	QVERIFY(QDir().mkpath(corrupt));
	QFile current(
		QDir(corrupt).filePath(QStringLiteral("current.json")));
	QVERIFY(current.open(QIODevice::WriteOnly));
	current.write("{");
	current.close();
	QVERIFY(QDir().mkpath(
		QDir(store.rootPath()).filePath(
			QStringLiteral(".staging-ignored"))));

	QVector<imagery::ImageryCatalogStoreIssue> issues;
	auto const installed = store.catalogs(&issues);
	QCOMPARE(installed.size(), 1);
	QCOMPARE(issues.size(), 1);
	QVERIFY(!issues.first().message.isEmpty());
}


void ImageryCatalogStoreTest::loadsAndMigratesLegacyLayout()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::ImageryCatalogStore store(
		directory.filePath(QStringLiteral("catalogs")));
	auto const candidate = catalog();
	auto const catalog_directory =
		QDir(store.rootPath()).filePath(
			store.directoryKey(candidate.catalog.id));
	QVERIFY(QDir().mkpath(catalog_directory));

	QFile catalog_file(
		QDir(catalog_directory).filePath(
			QStringLiteral("catalog.oic")));
	QVERIFY(catalog_file.open(QIODevice::WriteOnly));
	QCOMPARE(
		catalog_file.write(candidate.catalog.original_bytes),
		candidate.catalog.original_bytes.size());
	catalog_file.close();
	auto const installed_at =
		QDateTime::currentDateTimeUtc();
	QFile state_file(
		QDir(catalog_directory).filePath(
			QStringLiteral("state.json")));
	QVERIFY(state_file.open(QIODevice::WriteOnly));
	state_file.write(
		QJsonDocument(QJsonObject {
			{ QStringLiteral("origin"), QStringLiteral("legacy") },
			{ QStringLiteral("installedAt"),
			  installed_at.toString(Qt::ISODateWithMs) },
			{ QStringLiteral("sha256"),
			  QString::fromLatin1(
				  candidate.catalog.document_sha256) },
			{ QStringLiteral("etag"), QStringLiteral("old-etag") },
			{ QStringLiteral("lastModified"),
			  QStringLiteral("old-date") },
		}).toJson(QJsonDocument::Compact));
	state_file.close();

	auto installed = store.catalogs();
	QCOMPARE(installed.size(), 1);
	QVERIFY(installed.first().state.legacy_layout);
	QCOMPARE(installed.first().state.etag, QByteArray("old-etag"));

	QString error;
	QVERIFY(store.markChecked(
		candidate.catalog.id,
		QStringLiteral("legacy"),
		QByteArray("new-etag"),
		QByteArray("new-date"),
		&error));
	installed = store.catalogs();
	QCOMPARE(installed.size(), 1);
	QVERIFY(!installed.first().state.legacy_layout);
	QVERIFY(QFileInfo::exists(currentPath(
		store, candidate.catalog.id)));
	QVERIFY(QFileInfo::exists(installed.first().catalog_path));
}


void ImageryCatalogStoreTest::
	rejectsUnsafeRootAndRemovesOnlyCatalogEntry()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto const root_file =
		directory.filePath(QStringLiteral("not-a-directory"));
	QFile sentinel(root_file);
	QVERIFY(sentinel.open(QIODevice::WriteOnly));
	sentinel.write("sentinel");
	sentinel.close();

	imagery::ImageryCatalogStore unsafe_store(root_file);
	QString error;
	QVERIFY(!unsafe_store.install(
		catalog(), QStringLiteral("local"), {}, {}, &error));
	QVERIFY(!error.isEmpty());
	QVERIFY(sentinel.open(QIODevice::ReadOnly));
	QCOMPARE(sentinel.readAll(), QByteArray("sentinel"));

	imagery::ImageryCatalogStore store(
		directory.filePath(QStringLiteral("catalogs")));
	auto const candidate = catalog();
	QVERIFY(store.install(
		candidate, QStringLiteral("local"), {}, {}, &error));
	QFile unrelated(
		directory.filePath(QStringLiteral("unrelated.txt")));
	QVERIFY(unrelated.open(QIODevice::WriteOnly));
	unrelated.write("keep");
	unrelated.close();
	QVERIFY(store.remove(candidate.catalog.id, &error));
	QCOMPARE(store.catalogs().size(), 0);
	QVERIFY(unrelated.exists());
}


QTEST_APPLESS_MAIN(ImageryCatalogStoreTest)
