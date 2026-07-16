/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "online_imagery_dialog_t.h"

#include <QComboBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>

#include "gui/imagery/online_imagery_dialog.h"
#include "imagery/imagery_catalog_store.h"

using namespace OpenOrienteering;

namespace {

imagery::TileNetworkManager::Config configFor(
	const QTemporaryDir& directory)
{
	imagery::TileNetworkManager::Config config;
	config.cache_directory =
		directory.filePath(QStringLiteral("cache"));
	config.max_retries = 0;
	return config;
}

QByteArray catalogBytes()
{
	return QJsonDocument(QJsonObject {
		{ QStringLiteral("format"),
		  QStringLiteral("org.openorienteering.imagery-catalog") },
		{ QStringLiteral("version"), 1 },
		{ QStringLiteral("id"),
		  QStringLiteral("org.example.dialog") },
		{ QStringLiteral("revision"), 1 },
		{ QStringLiteral("name"), QStringLiteral("Dialog catalog") },
		{ QStringLiteral("sources"), QJsonArray {
			QJsonObject {
				{ QStringLiteral("id"), QStringLiteral("aerial") },
				{ QStringLiteral("name"),
				  QStringLiteral("Catalog aerial") },
				{ QStringLiteral("description"),
				  QStringLiteral("<b>Untrusted markup</b>") },
				{ QStringLiteral("type"),
				  QStringLiteral("raster-tiles") },
				{ QStringLiteral("tiles"), QJsonArray {
					QStringLiteral(
						"https://catalog.example.test/"
						"{z}/{x}/{y}.png"),
				} },
				{ QStringLiteral("scheme"), QStringLiteral("xyz") },
				{ QStringLiteral("tileMatrixSetURI"),
				  QStringLiteral(
					  "http://www.opengis.net/def/tilematrixset/"
					  "OGC/1.0/WebMercatorQuad") },
			},
		} },
	}).toJson(QJsonDocument::Compact);
}

}  // namespace


void OnlineImageryDialogTest::resolvesDirectManualSource()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(
		directory.filePath(QStringLiteral("catalogs")),
		&network);
	OnlineImageryDialog dialog(repository, network);
	auto* url = dialog.findChild<QLineEdit*>(
		QStringLiteral("manual_imagery_url"));
	auto* add = dialog.findChild<QPushButton*>(
		QStringLiteral("add_online_imagery"));
	QVERIFY(url);
	QVERIFY(add);

	url->setFocus();
	QTest::keyClicks(
		url,
		QStringLiteral(
			"https://tiles.example.test/${z}/${x}/${y}.png"));
	QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 1500);
	auto const& source = dialog.selectedSource();
	QCOMPARE(source.row_scheme, imagery::TileRowScheme::Xyz);
	QCOMPARE(source.min_zoom, 0);
	QCOMPARE(source.max_zoom, 19);
	QCOMPARE(
		source.tile_urls.first().value,
		QStringLiteral(
			"https://tiles.example.test/{z}/{x}/{y}.png"));
	QCOMPARE(
		source.tile_matrix_set.matrices.first().tile_size,
		QSize(256, 256));
	QVERIFY(!dialog.displayName().isEmpty());
}


void OnlineImageryDialogTest::exposesAdvancedTmsAndTileSize()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(
		directory.filePath(QStringLiteral("catalogs")),
		&network);
	OnlineImageryDialog dialog(repository, network);
	auto* url = dialog.findChild<QLineEdit*>(
		QStringLiteral("manual_imagery_url"));
	auto* scheme = dialog.findChild<QComboBox*>(
		QStringLiteral("manual_row_scheme"));
	auto* tile_size = dialog.findChild<QComboBox*>(
		QStringLiteral("manual_tile_size"));
	auto* minimum = dialog.findChild<QSpinBox*>(
		QStringLiteral("manual_min_zoom"));
	auto* maximum = dialog.findChild<QSpinBox*>(
		QStringLiteral("manual_max_zoom"));
	auto* add = dialog.findChild<QPushButton*>(
		QStringLiteral("add_online_imagery"));
	QVERIFY(url);
	QVERIFY(scheme);
	QVERIFY(tile_size);
	QVERIFY(minimum);
	QVERIFY(maximum);
	QVERIFY(add);

	scheme->setCurrentIndex(1);
	tile_size->setCurrentIndex(1);
	minimum->setValue(3);
	maximum->setValue(12);
	url->setFocus();
	QTest::keyClicks(
		url,
		QStringLiteral(
			"https://tiles.example.test/{z}/{x}/{y}.png"));
	QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 1500);
	auto const& source = dialog.selectedSource();
	QCOMPARE(source.row_scheme, imagery::TileRowScheme::Tms);
	QCOMPARE(source.min_zoom, 3);
	QCOMPARE(source.max_zoom, 12);
	QCOMPARE(
		source.tile_matrix_set.matrices.first().tile_size,
		QSize(512, 512));
}


void OnlineImageryDialogTest::
	selectsCatalogSourceByStableHandle()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto const root =
		directory.filePath(QStringLiteral("catalogs"));
	auto const read =
		imagery::OicCatalogReader::read(catalogBytes());
	QVERIFY(read.accepted());
	imagery::ImageryCatalogStore store(root);
	QString error;
	QVERIFY2(
		store.install(
			read,
			QStringLiteral("local"),
			{},
			{},
			&error),
		qPrintable(error));

	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(root, &network);
	QSignalSpy snapshots(
		&repository,
		&imagery::ImageryCatalogRepository::snapshotChanged);
	repository.reload();
	QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty(), 3000);

	OnlineImageryDialog dialog(repository, network);
	auto* tree = dialog.findChild<QTreeView*>(
		QStringLiteral("imagery_source_tree"));
	auto* add = dialog.findChild<QPushButton*>(
		QStringLiteral("add_online_imagery"));
	QVERIFY(tree);
	QVERIFY(add);
	auto const catalog = tree->model()->index(0, 0);
	QVERIFY(catalog.isValid());
	auto const source = tree->model()->index(0, 0, catalog);
	QVERIFY(source.isValid());
	tree->setCurrentIndex(source);
	QTRY_VERIFY_WITH_TIMEOUT(add->isEnabled(), 1000);
	QCOMPARE(
		dialog.selectedSource().metadata.id,
		QStringLiteral("aerial"));
	QVERIFY(
		dialog.selectedSource().catalog_provenance.has_value());
	QCOMPARE(
		dialog.selectedSource()
			.catalog_provenance->catalog_id,
		QStringLiteral("org.example.dialog"));
	QCOMPARE(dialog.displayName(), QStringLiteral("Catalog aerial"));
	auto* description = dialog.findChild<QLabel*>(
		QStringLiteral("imagery_detail_description"));
	QVERIFY(description);
	QCOMPARE(description->textFormat(), Qt::PlainText);
	QCOMPARE(
		description->text(),
		QStringLiteral("<b>Untrusted markup</b>"));
}


void OnlineImageryDialogTest::
	switchingToCatalogCancelsStaleDiscovery()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto const root =
		directory.filePath(QStringLiteral("catalogs"));
	auto const read =
		imagery::OicCatalogReader::read(catalogBytes());
	QVERIFY(read.accepted());
	imagery::ImageryCatalogStore store(root);
	QString error;
	QVERIFY2(
		store.install(
			read,
			QStringLiteral("local"),
			{},
			{},
			&error),
		qPrintable(error));

	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(root, &network);
	QSignalSpy snapshots(
		&repository,
		&imagery::ImageryCatalogRepository::snapshotChanged);
	repository.reload();
	QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty(), 3000);

	OnlineImageryDialog dialog(repository, network);
	auto* tree = dialog.findChild<QTreeView*>(
		QStringLiteral("imagery_source_tree"));
	QVERIFY(tree);
	auto const catalog = tree->model()->index(0, 0);
	QVERIFY(catalog.isValid());
	auto const source =
		tree->model()->index(0, 0, catalog);
	QVERIFY(source.isValid());
	tree->setCurrentIndex(source);
	QTRY_VERIFY_WITH_TIMEOUT(
		dialog.selected_handle_.has_value(),
		1000);
	auto const handle = *dialog.selected_handle_;
	dialog.pages_->setCurrentIndex(1);
	dialog.discovery_generation_ = 41;
	dialog.discovery_token_ = 987654;
	dialog.showCatalogSource(handle);
	QCOMPARE(dialog.pages_->currentIndex(), 0);
	QCOMPARE(dialog.discovery_token_, quint64(0));
	QCOMPARE(dialog.discovery_generation_, quint64(42));
	QCOMPARE(
		dialog.selectedSource().metadata.id,
		QStringLiteral("aerial"));

	imagery::TileNetworkResult stale;
	stale.outcome =
		imagery::TileNetworkResult::Outcome::Success;
	stale.client_id = dialog.network_client_id_;
	stale.generation = 41;
	stale.body = QByteArrayLiteral("{}");
	dialog.onNetworkFinished(987654, stale);
	QCOMPARE(
		dialog.selectedSource().metadata.id,
		QStringLiteral("aerial"));
}


void OnlineImageryDialogTest::
	arcGisDiscoveryUsesConfiguredReferer()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(
		directory.filePath(QStringLiteral("catalogs")),
		&network);
	OnlineImageryDialog dialog(repository, network);
	auto* referer = dialog.findChild<QLineEdit*>(
		QStringLiteral("manual_referer"));
	QVERIFY(referer);
	referer->setText(
		QStringLiteral("https://portal.example.test/maps?id=one two"));
	dialog.manual_result_.discovery_url = QUrl(
		QStringLiteral("https://tiles.example.test/MapServer?f=pjson"));

	auto const request = dialog.arcGisDiscoveryRequest(17);
	QCOMPARE(request.url, dialog.manual_result_.discovery_url);
	QCOMPARE(request.client_id, dialog.network_client_id_);
	QCOMPARE(request.generation, quint64(17));
	QCOMPARE(
		request.referer,
		QStringLiteral(
			"https://portal.example.test/maps?id=one%20two"));
	QCOMPARE(
		request.payload_kind,
		imagery::NetworkPayloadKind::JsonDocument);
}


QTEST_MAIN(OnlineImageryDialogTest)
