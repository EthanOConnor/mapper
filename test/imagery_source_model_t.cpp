/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "imagery_source_model_t.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSortFilterProxyModel>
#include <QTemporaryDir>
#include <QtTest>

#include "gui/imagery/imagery_source_model.h"
#include "imagery/imagery_catalog_store.h"

using namespace OpenOrienteering;

namespace {

QByteArray catalogBytes(
	int revision,
	QString source_name,
	QString source_description = QStringLiteral("Spring orthophoto"))
{
	return QJsonDocument(QJsonObject {
		{ QStringLiteral("format"),
		  QStringLiteral("org.openorienteering.imagery-catalog") },
		{ QStringLiteral("version"), 1 },
		{ QStringLiteral("id"),
		  QStringLiteral("org.example.model") },
		{ QStringLiteral("revision"), revision },
		{ QStringLiteral("name"), QStringLiteral("Model catalog") },
		{ QStringLiteral("sources"), QJsonArray {
			QJsonObject {
				{ QStringLiteral("id"), QStringLiteral("aerial") },
				{ QStringLiteral("name"), source_name },
				{ QStringLiteral("description"),
				  source_description },
				{ QStringLiteral("type"),
				  QStringLiteral("raster-tiles") },
				{ QStringLiteral("tiles"), QJsonArray {
					QStringLiteral(
						"https://tiles.example.test/"
						"{z}/{x}/{y}.png"),
				} },
				{ QStringLiteral("scheme"), QStringLiteral("xyz") },
				{ QStringLiteral("tileMatrixSetURI"),
				  QStringLiteral(
					  "http://www.opengis.net/def/tilematrixset/"
					  "OGC/1.0/WebMercatorQuad") },
			},
			QJsonObject {
				{ QStringLiteral("id"), QStringLiteral("future") },
				{ QStringLiteral("name"),
				  QStringLiteral("Future grid") },
				{ QStringLiteral("type"),
				  QStringLiteral("raster-tiles") },
				{ QStringLiteral("tiles"), QJsonArray {
					QStringLiteral(
						"https://future.example.test/"
						"{z}/{x}/{y}.png"),
				} },
				{ QStringLiteral("scheme"), QStringLiteral("xyz") },
				{ QStringLiteral("tileMatrixSet"), QJsonObject {
					{ QStringLiteral("id"),
					  QStringLiteral("future-grid") },
					{ QStringLiteral("crs"),
					  QStringLiteral("EPSG:3857") },
					{ QStringLiteral("orderedAxes"),
					  QJsonArray {
						  QStringLiteral("E"),
						  QStringLiteral("N"),
					  } },
					{ QStringLiteral("tileMatrices"), QJsonArray {
						QJsonObject {
							{ QStringLiteral("id"),
							  QStringLiteral("0") },
							{ QStringLiteral("scaleDenominator"),
							  559082264.0287178 },
							{ QStringLiteral("cellSize"),
							  156543.03392804097 },
							{ QStringLiteral("pointOfOrigin"),
							  QJsonArray {
								  -20037508.342789244,
								  20037508.342789244,
							  } },
							{ QStringLiteral("cornerOfOrigin"),
							  QStringLiteral("bottomLeft") },
							{ QStringLiteral("tileWidth"), 256 },
							{ QStringLiteral("tileHeight"), 256 },
							{ QStringLiteral("matrixWidth"), 1 },
							{ QStringLiteral("matrixHeight"), 1 },
						},
					} },
				} },
			},
		} },
	}).toJson(QJsonDocument::Compact);
}

imagery::TileNetworkManager::Config configFor(
	const QTemporaryDir& directory)
{
	imagery::TileNetworkManager::Config config;
	config.cache_directory =
		directory.filePath(QStringLiteral("cache"));
	config.max_retries = 0;
	return config;
}

void install(
	const QString& root,
	int revision,
	const QString& name,
	imagery::ImageryCatalogInstallOptions options = {},
	const QString& origin = QStringLiteral("local"),
	const QString& source_description =
		QStringLiteral("Spring orthophoto"))
{
	auto const read = imagery::OicCatalogReader::read(
		catalogBytes(revision, name, source_description));
	QVERIFY(read.accepted());
	imagery::ImageryCatalogStore store(root);
	QString error;
	QVERIFY2(
		store.install(
			read,
			imagery::ImageryCatalogInstallMetadata {
				origin,
				origin,
				{},
				{},
			},
			options,
			&error),
		qPrintable(error));
}

}  // namespace


void ImagerySourceModelTest::
	publishesHierarchyAndRecursiveSearch()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto const root =
		directory.filePath(QStringLiteral("catalogs"));
	install(root, 1, QStringLiteral("Spring aerial"));
	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(root, &network);
	ImagerySourceModel model(repository);
	QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
	repository.reload();
	QTRY_VERIFY_WITH_TIMEOUT(!resets.isEmpty(), 3000);

	QCOMPARE(model.rowCount(), 1);
	auto const catalog = model.index(0, 0);
	QCOMPARE(model.rowCount(catalog), 2);
	QVERIFY(!model.data(
		catalog,
		ImagerySourceModel::StatusTextRole).toString().isEmpty());
	auto first = model.index(0, 0, catalog);
	auto second = model.index(1, 0, catalog);
	if (model.data(first).toString() != QLatin1String("Spring aerial"))
		std::swap(first, second);
	QCOMPARE(
		model.data(first, ImagerySourceModel::SupportedRole).toBool(),
		true);
	QCOMPARE(
		model.data(second, ImagerySourceModel::SupportedRole).toBool(),
		false);
	auto const handle = model.sourceHandle(first);
	QVERIFY(handle);
	QVERIFY(handle->isValid());

	QSortFilterProxyModel filter;
	filter.setSourceModel(&model);
	filter.setFilterRole(ImagerySourceModel::SearchTextRole);
	filter.setFilterCaseSensitivity(Qt::CaseInsensitive);
	filter.setRecursiveFilteringEnabled(true);
	filter.setFilterFixedString(QStringLiteral("orthophoto"));
	QCOMPARE(filter.rowCount(), 1);
	QCOMPARE(filter.rowCount(filter.index(0, 0)), 1);
}

void ImagerySourceModelTest::catalogOriginTooltipRedactsSecrets()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto const root =
		directory.filePath(QStringLiteral("catalogs"));
	install(
		root,
		1,
		QStringLiteral("Spring aerial"),
		{},
		QStringLiteral(
			"https://publisher:password@example.test/catalog.oic"
			"?token=top-secret#fragment"));
	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(root, &network);
	ImagerySourceModel model(repository);
	QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
	repository.reload();
	QTRY_VERIFY_WITH_TIMEOUT(!resets.isEmpty(), 3000);

	auto const tooltip =
		model.data(model.index(0, 0), Qt::ToolTipRole).toString();
	QVERIFY(tooltip.contains(
		QStringLiteral("example.test/catalog.oic")));
	QVERIFY(!tooltip.contains(QStringLiteral("publisher")));
	QVERIFY(!tooltip.contains(QStringLiteral("password")));
	QVERIFY(!tooltip.contains(QStringLiteral("top-secret")));
	QVERIFY(!tooltip.contains(QStringLiteral("fragment")));
}


void ImagerySourceModelTest::catalogTooltipsEscapeRichText()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto const root =
		directory.filePath(QStringLiteral("catalogs"));
	auto const malicious = QStringLiteral(
		"<img src='https://tracker.example.test/pixel'>"
		"<b>Catalog supplied text</b>");
	install(
		root,
		1,
		QStringLiteral("Spring aerial"),
		{},
		QStringLiteral("local"),
		malicious);
	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(root, &network);
	ImagerySourceModel model(repository);
	QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
	repository.reload();
	QTRY_VERIFY_WITH_TIMEOUT(!resets.isEmpty(), 3000);

	auto const catalog = model.index(0, 0);
	auto source = model.index(0, 0, catalog);
	if (model.data(source).toString() != QLatin1String("Spring aerial"))
		source = model.index(1, 0, catalog);
	auto const tooltip =
		model.data(source, Qt::ToolTipRole).toString();
	QVERIFY(tooltip.startsWith(QLatin1String("<qt>")));
	QVERIFY(tooltip.contains(QLatin1String("&lt;img")));
	QVERIFY(tooltip.contains(QLatin1String("&lt;b&gt;")));
	QVERIFY(!tooltip.contains(QLatin1String("<img")));
	QVERIFY(!tooltip.contains(QLatin1String("<b>")));
}


void ImagerySourceModelTest::stableHandlesDetectCatalogUpdates()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto const root =
		directory.filePath(QStringLiteral("catalogs"));
	install(root, 1, QStringLiteral("First name"));
	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(root, &network);
	ImagerySourceModel model(repository);
	QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
	repository.reload();
	QTRY_VERIFY_WITH_TIMEOUT(!resets.isEmpty(), 3000);

	auto old_handle = repository.snapshot()->latestHandle(
		QStringLiteral("org.example.model"),
		QStringLiteral("aerial"));
	QVERIFY(old_handle);
	QVERIFY(model.indexForHandle(*old_handle).isValid());

	install(root, 2, QStringLiteral("Updated name"));
	auto const previous_resets = resets.size();
	repository.reload();
	QTRY_VERIFY_WITH_TIMEOUT(
		resets.size() > previous_resets,
		3000);
	QVERIFY(!repository.snapshot()->source(*old_handle));
	auto const new_handle = repository.snapshot()->latestHandle(
		QStringLiteral("org.example.model"),
		QStringLiteral("aerial"));
	QVERIFY(new_handle);
	QVERIFY(*new_handle != *old_handle);
	QVERIFY(model.indexForHandle(*new_handle).isValid());
}


void ImagerySourceModelTest::invalidSourcesUseStableIndexIdentity()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto const root =
		directory.filePath(QStringLiteral("catalogs"));
	auto object =
		QJsonDocument::fromJson(
			catalogBytes(1, QStringLiteral("Spring aerial")))
			.object();
	auto sources =
		object.value(QStringLiteral("sources")).toArray();
	sources.push_back(QJsonObject {
		{ QStringLiteral("name"),
		  QStringLiteral("Missing identity") },
		{ QStringLiteral("type"),
		  QStringLiteral("raster-tiles") },
		{ QStringLiteral("tiles"), QJsonArray {
			QStringLiteral(
				"https://invalid.example.test/{z}/{x}/{y}.png"),
		} },
		{ QStringLiteral("scheme"), QStringLiteral("xyz") },
		{ QStringLiteral("tileMatrixSetURI"),
		  QStringLiteral(
			  "http://www.opengis.net/def/tilematrixset/"
			  "OGC/1.0/WebMercatorQuad") },
	});
	object.insert(QStringLiteral("sources"), sources);
	auto const read = imagery::OicCatalogReader::read(
		QJsonDocument(object).toJson(QJsonDocument::Compact));
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
	ImagerySourceModel model(repository);
	QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
	repository.reload();
	QTRY_VERIFY_WITH_TIMEOUT(!resets.isEmpty(), 3000);

	auto const catalog = model.index(0, 0);
	QModelIndex invalid;
	for (int row = 0; row < model.rowCount(catalog); ++row)
	{
		auto const candidate = model.index(row, 0, catalog);
		if (model.data(candidate).toString()
		      == QLatin1String("Missing identity"))
		{
			invalid = candidate;
			break;
		}
	}
	QVERIFY(invalid.isValid());
	auto const handle = model.sourceHandle(invalid);
	QVERIFY(handle);
	QVERIFY(handle->isValid());
	QVERIFY(handle->source_id.isEmpty());
	QCOMPARE(handle->source_index, 2);
	auto const* definition =
		repository.snapshot()->source(*handle);
	QVERIFY(definition);
	QCOMPARE(
		definition->metadata.name,
		QStringLiteral("Missing identity"));
	QCOMPARE(model.indexForHandle(*handle), invalid);
}


QTEST_MAIN(ImagerySourceModelTest)
