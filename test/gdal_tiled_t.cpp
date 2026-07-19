/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include <algorithm>
#include <memory>
#include <vector>

#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#include "global.h"
#include "core/map.h"
#include "gdal/gdal_image_reader.h"
#include "gdal/gdal_manager.h"
#include "gdal/gdal_template.h"

namespace OpenOrienteering {

namespace {

QString createGeoTiff(const QString& path,
	                  QSize size,
	                  QSize blocks,
	                  bool georeferenced)
{
	auto* driver = GDALGetDriverByName("GTiff");
	if (!driver)
		return {};

	char** options = nullptr;
	if (!blocks.isEmpty())
	{
		options = CSLSetNameValue(options, "TILED", "YES");
		options = CSLSetNameValue(
			options, "BLOCKXSIZE", QByteArray::number(blocks.width()).constData()
		);
		options = CSLSetNameValue(
			options, "BLOCKYSIZE", QByteArray::number(blocks.height()).constData()
		);
	}
	auto* dataset = GDALCreate(
		driver, path.toUtf8().constData(), size.width(), size.height(), 1, GDT_Byte, options
	);
	CSLDestroy(options);
	if (!dataset)
		return {};

	auto* band = GDALGetRasterBand(dataset, 1);
	GDALSetRasterColorInterpretation(band, GCI_GrayIndex);
	std::vector<GByte> row(std::size_t(size.width()));
	for (int y = 0; y < size.height(); ++y)
	{
		for (int x = 0; x < size.width(); ++x)
			row[std::size_t(x)] = GByte((x + y) % 256);
		if (GDALRasterIO(
			band, GF_Write, 0, y, size.width(), 1, row.data(), size.width(), 1,
			GDT_Byte, 0, 0
		) >= CE_Warning)
		{
			GDALClose(dataset);
			return {};
		}
	}

	if (georeferenced)
	{
		double transform[6] = { 410000.0, 2.0, 0.0, 5300000.0, 0.0, -2.0 };
		if (GDALSetGeoTransform(dataset, transform) >= CE_Warning)
		{
			GDALClose(dataset);
			return {};
		}
		auto* srs = OSRNewSpatialReference(nullptr);
		if (!srs || OSRSetFromUserInput(srs, "EPSG:3857") != OGRERR_NONE)
		{
			OSRDestroySpatialReference(srs);
			GDALClose(dataset);
			return {};
		}
		char* wkt = nullptr;
		auto const exported = OSRExportToWkt(srs, &wkt) == OGRERR_NONE;
		OSRDestroySpatialReference(srs);
		if (!exported || GDALSetProjection(dataset, wkt) >= CE_Warning)
		{
			CPLFree(wkt);
			GDALClose(dataset);
			return {};
		}
		CPLFree(wkt);
	}

	GDALClose(dataset);
	return path;
}

}  // namespace

class GdalTiledTest : public QObject
{
Q_OBJECT

private slots:
	void initTestCase()
	{
		QCoreApplication::setOrganizationName(QStringLiteral("OpenOrienteering.org"));
		QCoreApplication::setApplicationName(QStringLiteral("GdalTiledTest"));
		doStaticInitializations();
		GdalManager();
	}

	void sourceDetectionUsesLogicalTilesForLargeStrips()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto const native = createGeoTiff(
			dir.filePath(QStringLiteral("native.tif")), { 256, 256 }, { 64, 64 }, true
		);
		QVERIFY(!native.isEmpty());
		QCOMPARE(GdalImageReader(native).readRasterInfo().block_size, QSize(64, 64));

		auto const large = createGeoTiff(
			dir.filePath(QStringLiteral("large-strip.tif")), { 4200, 4200 }, {}, false
		);
		QVERIFY(!large.isEmpty());
		QCOMPARE(GdalImageReader(large).readRasterInfo().block_size, QSize(512, 512));
	}

	void tileMathIncludesSamplingGutters()
	{
		QCOMPARE(
			GdalTemplate::sourceRectForTile({ 130, 130 }, { 64, 64 }, 1, 0, 1),
			QRect(64, 0, 64, 64)
		);
		QCOMPARE(
			GdalTemplate::decodedSourceRectForTile({ 130, 130 }, { 64, 64 }, 1, 0, 1),
			QRect(63, 0, 66, 65)
		);
		QCOMPARE(
			GdalTemplate::decodedSourceRectForTile({ 130, 130 }, { 64, 64 }, 0, 0, 2),
			QRect(0, 0, 130, 130)
		);
		QCOMPARE(GdalTemplate::chooseTileSubsampling(0.5, { 64, 64 }), 2);
		QCOMPARE(GdalTemplate::chooseTileSubsampling(0.25, { 64, 64 }), 4);
	}

	void threadSafeDatasetDecodesOnQtPool()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		auto const path = createGeoTiff(
			dir.filePath(QStringLiteral("thread-safe.tif")), { 256, 256 }, { 64, 64 }, true
		);
		QVERIFY(!path.isEmpty());

		Map map;
		GdalTemplate source(path, &map);
		QVERIFY(source.loadTemplateFileImpl());
		QVERIFY(source.isTiledSource());
		QVERIFY(source.tiled_dataset->IsThreadSafe(GDAL_OF_RASTER));
		QVERIFY(source.tile_pool.maxThreadCount() >= 1);
		QVERIFY(source.tile_pool.maxThreadCount() <= 4);

		GdalTemplate::TileWindow window { 0, 0, 1, 1, 1 };
		source.queueWantedTiles(window, true);
		QTRY_VERIFY_WITH_TIMEOUT(source.tile_cache.contains(GdalTemplate::tileKey(0, 0, 1)), 5000);
		QTRY_VERIFY_WITH_TIMEOUT(source.tile_cache.contains(GdalTemplate::tileKey(1, 1, 1)), 5000);
		auto const* tile = source.tile_cache.object(GdalTemplate::tileKey(0, 0, 1));
		QVERIFY(tile);
		QCOMPARE(tile->size(), QSize(65, 65));
		QVERIFY(source.tile_cache.totalCost() <= source.tile_cache.maxCost());

		source.setTemplateState(Template::Loaded);
		auto duplicate = std::unique_ptr<GdalTemplate>(source.duplicate());
		QCOMPARE(duplicate->getTemplateState(), Template::Loaded);
		QVERIFY(duplicate->isTiledSource());
	}

	void screenQueueIsBoundedAndStartsWithCoarseCoverage()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		auto const path = createGeoTiff(
			dir.filePath(QStringLiteral("queue.tif")), { 1024, 1024 }, { 64, 64 }, false
		);
		QVERIFY(!path.isEmpty());

		Map map;
		GdalTemplate source(path, &map);
		QVERIFY(source.loadTemplateFileImpl());
		source.tile_pool.setMaxThreadCount(1);
		source.queueWantedTiles({ 0, 0, 15, 15, 1 }, true);
		QVERIFY(source.queued_tiles.size() <= 64);
		QTRY_VERIFY_WITH_TIMEOUT(
			source.tile_cache.contains(GdalTemplate::tileKey(0, 0, 64)), 5000
		);
		QVERIFY(source.queued_tiles.size() <= 64);
	}

	void oversizedScreenWindowConvergesWithinAdmissionBudget()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		auto const path = createGeoTiff(
			dir.filePath(QStringLiteral("oversized-window.tif")),
			{ 2048, 2048 }, { 64, 64 }, false
		);
		QVERIFY(!path.isEmpty());

		Map map;
		GdalTemplate source(path, &map);
		QVERIFY(source.loadTemplateFileImpl());
		source.tile_pool.setMaxThreadCount(4);
		// Force every decoded tile out of the cache. A stable scheduler must
		// still attempt each admitted key at most once for this view.
		source.tile_cache.setMaxCost(1);
		GdalTemplate::TileWindow window { 0, 0, 31, 31, 1 };
		source.wanted_window = window;
		auto const admission_budget = source.screenTileAdmissionBudget();
		QCOMPARE(admission_budget, 64);
		source.queueWantedTiles(window, true);

		QTRY_COMPARE_WITH_TIMEOUT(
			source.attempted_tiles.size(), admission_budget, 15000
		);
		QTRY_VERIFY_WITH_TIMEOUT(source.queued_tiles.isEmpty(), 15000);
		QVERIFY(source.attempted_window == window);
		QVERIFY(source.tile_cache.totalCost() <= source.tile_cache.maxCost());

		auto const stable_attempts = source.attempted_tiles;
		QTest::qWait(250);
		QVERIFY(source.attempted_tiles == stable_attempts);
		QVERIFY(source.queued_tiles.isEmpty());
	}

	void oddTmsOriginNeverQueuesMisalignedFallbacks()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		auto const path = createGeoTiff(
			dir.filePath(QStringLiteral("odd-origin.tif")),
			{ 1024, 1024 }, { 64, 64 }, false
		);
		QVERIFY(!path.isEmpty());

		Map map;
		GdalTemplate source(path, &map);
		QVERIFY(source.loadTemplateFileImpl());
		source.tile_pool.setMaxThreadCount(1);
		source.has_tiled_origin_tile = true;
		source.tiled_origin_tile = { 3, 5 };
		QCOMPARE(source.chooseTiledSubsampling(0.25), 1);
		source.tile_cache.setMaxCost(1);
		QVERIFY(source.screenTileWindowForMapRect(
			source.getTemplateExtent(), 1
		).isEmpty());

		source.queueWantedTiles({ 0, 0, 15, 15, 1 }, true);
		QVERIFY(!source.attempted_tiles.isEmpty());
		for (auto const& key : source.attempted_tiles)
			QCOMPARE(key.subsampling, 1);

		source.shutdownTiledSource();
		QVERIFY(source.attempted_tiles.isEmpty());
	}

	void oversizedScreenWindowUsesCoarsestFittingOverview()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		auto const path = createGeoTiff(
			dir.filePath(QStringLiteral("adaptive-overview.tif")),
			{ 2048, 2048 }, { 64, 64 }, false
		);
		QVERIFY(!path.isEmpty());

		Map map;
		GdalTemplate source(path, &map);
		QVERIFY(source.loadTemplateFileImpl());
		source.tile_cache.setMaxCost(1);
		auto const window = source.screenTileWindowForMapRect(
			source.getTemplateExtent(), 1
		);
		QVERIFY(!window.isEmpty());
		QCOMPARE(window.subsampling, 4);
		QCOMPARE(window.tileCount(), 64);
	}

	void nonGeoreferencedLargeRasterStaysTiled()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());
		auto const path = createGeoTiff(
			dir.filePath(QStringLiteral("plain-large.tif")), { 4200, 4200 }, {}, false
		);
		QVERIFY(!path.isEmpty());

		Map map;
		GdalTemplate source(path, &map);
		QVERIFY(source.loadTemplateFileImpl());
		QVERIFY(source.isTiledSource());
		QVERIFY(!source.isTemplateGeoreferenced());
		QCOMPARE(source.getTemplateExtent(), QRectF(-2100, -2100, 4200, 4200));
	}
};

}  // namespace OpenOrienteering

using OpenOrienteering::GdalTiledTest;

QTEST_MAIN(GdalTiledTest)
#include "gdal_tiled_t.moc"
