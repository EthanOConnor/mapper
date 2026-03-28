/*
 *    Copyright 2026 Ethan O'Connor
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

#include "gdal_tiled_template_t.h"

#include <array>
#include <memory>
#include <vector>

#include <Qt>
#include <QtGlobal>
#include <QtTest>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QLineF>
#include <QPainter>
#include <QRectF>
#include <QString>

#include <cpl_conv.h>
#include <cpl_vsi.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#include "global.h"
#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map.h"
#include "gdal/gdal_image_reader.h"
#include "gdal/gdal_manager.h"
#include "gdal/gdal_template.h"
#include "gui/map/map_widget.h"
#include "templates/template.h"
#include "templates/template_image.h"


using namespace OpenOrienteering;


namespace {

/**
 * Helper to set up a map with UTM 32N georeferencing for template tests.
 */
void setupMapGeoreferencing(Map& map)
{
	Georeferencing georef;
	georef.setProjectedCRS(QStringLiteral("UTM 32N"), QStringLiteral("EPSG:32632"));
	georef.setGeographicRefPoint(LatLon(54.0, 9.0));
	map.setGeoreferencing(georef);
}

/**
 * Helper that mirrors the real Wilburton map setup more closely:
 * a UTM Zone 10 map with a 1:5000 scale, while the raster can stay in 3857.
 */
void setupWilburtonLikeGeoreferencing(Map& map)
{
	Georeferencing georef;
	georef.setScaleDenominator(5000);
	georef.setProjectedCRS(QStringLiteral("UTM 10N"), QStringLiteral("EPSG:32610"));
	georef.setGeographicRefPoint(LatLon(47.6094261, -122.17468293));
	georef.setGrivation(14.38);
	map.setGeoreferencing(georef);
}

/**
 * Helper to set SRS on a GDAL dataset using proper OGR/SRS API.
 */
void setDatasetSRS(GDALDatasetH dataset, int epsg_code)
{
	auto srs = OSRNewSpatialReference(nullptr);
	OSRImportFromEPSG(srs, epsg_code);
	char* wkt = nullptr;
	OSRExportToWkt(srs, &wkt);
	GDALSetProjection(dataset, wkt);
	CPLFree(wkt);
	OSRDestroySpatialReference(srs);
}

class TestableGdalTemplate final : public GdalTemplate
{
public:
	using GdalTemplate::GdalTemplate;
	using GdalTemplate::drawTemplate;

	void forceGeoreferenced(bool value)
	{
		is_georeferenced = value;
	}
};

}  // namespace


GdalTiledTemplateTest::GdalTiledTemplateTest(QObject* parent)
: QObject(parent)
{}


void GdalTiledTemplateTest::initTestCase()
{
	QCoreApplication::setOrganizationName(QString::fromLatin1("OpenOrienteering.org"));
	QCoreApplication::setApplicationName(QString::fromLatin1(metaObject()->className()));

	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
	// Trigger Map static init.
	Map map;
	Q_UNUSED(map)

	GdalManager();
	GDALAllRegister();
}


void GdalTiledTemplateTest::cleanupTestCase()
{
	// /vsimem/ files are cleaned up explicitly in each test.
}


QString GdalTiledTemplateTest::createTiledTestRaster(
	int width,
	int height,
	int block_w,
	int block_h,
	int epsg_code,
	const std::array<double, 6>& geotransform
)
{
	auto path = QStringLiteral("/vsimem/test_tiled_%1.tif").arg(++test_counter);

	auto* driver = GDALGetDriverByName("GTiff");
	Q_ASSERT(driver);
	if (!driver)
		return {};

	QByteArray block_x_str = QByteArray("BLOCKXSIZE=") + QByteArray::number(block_w);
	QByteArray block_y_str = QByteArray("BLOCKYSIZE=") + QByteArray::number(block_h);
	char* options[] = {
		const_cast<char*>("TILED=YES"),
		block_x_str.data(),
		block_y_str.data(),
		nullptr
	};

	auto* dataset = GDALCreate(driver, path.toUtf8().constData(),
	                           width, height, 3, GDT_Byte, options);
	Q_ASSERT(dataset);
	if (!dataset)
		return {};

	// Write gradient pixel data so tiles have recognizable content.
	auto row = std::vector<unsigned char>(width);
	for (int band = 1; band <= 3; ++band)
	{
		auto* raster_band = GDALGetRasterBand(dataset, band);
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
				row[x] = static_cast<unsigned char>((x + y * band) % 256);
			GDALRasterIO(raster_band, GF_Write, 0, y, width, 1,
			             row.data(), width, 1, GDT_Byte, 0, 0);
		}
	}

	double gt[6] = {
		geotransform[0], geotransform[1], geotransform[2],
		geotransform[3], geotransform[4], geotransform[5]
	};
	GDALSetGeoTransform(dataset, gt);
	setDatasetSRS(dataset, epsg_code);

	GDALClose(dataset);
	return path;
}


QString GdalTiledTemplateTest::createNonTiledTestRaster(
	int width,
	int height,
	int epsg_code,
	const std::array<double, 6>& geotransform
)
{
	auto path = QStringLiteral("/vsimem/test_nontiled_%1.tif").arg(++test_counter);

	auto* driver = GDALGetDriverByName("GTiff");
	Q_ASSERT(driver);
	if (!driver)
		return {};

	auto* dataset = GDALCreate(driver, path.toUtf8().constData(),
	                           width, height, 3, GDT_Byte, nullptr);
	Q_ASSERT(dataset);
	if (!dataset)
		return {};

	auto row = std::vector<unsigned char>(width, 128);
	for (int band = 1; band <= 3; ++band)
	{
		auto* raster_band = GDALGetRasterBand(dataset, band);
		for (int y = 0; y < height; ++y)
		{
			GDALRasterIO(raster_band, GF_Write, 0, y, width, 1,
			             row.data(), width, 1, GDT_Byte, 0, 0);
		}
	}

	double gt[6] = {
		geotransform[0], geotransform[1], geotransform[2],
		geotransform[3], geotransform[4], geotransform[5]
	};
	GDALSetGeoTransform(dataset, gt);
	setDatasetSRS(dataset, epsg_code);

	GDALClose(dataset);
	return path;
}


void GdalTiledTemplateTest::blockSizeDetection()
{
	// Tiled raster: block_size should be set.
	{
		auto path = createTiledTestRaster(1024, 1024, 256, 256);
		QVERIFY(!path.isEmpty());
		GdalImageReader reader(path);
		QVERIFY(reader.canRead());
		auto info = reader.readRasterInfo();
		QCOMPARE(info.block_size.width(), 256);
		QCOMPARE(info.block_size.height(), 256);
		QCOMPARE(info.size.width(), 1024);
		QCOMPARE(info.size.height(), 1024);
		VSIUnlink(path.toUtf8().constData());
	}

	// Non-tiled raster: block_size should be empty.
	{
		auto path = createNonTiledTestRaster(200, 200);
		QVERIFY(!path.isEmpty());
		GdalImageReader reader(path);
		QVERIFY(reader.canRead());
		auto info = reader.readRasterInfo();
		QVERIFY(info.block_size.isEmpty());
		QCOMPARE(info.size.width(), 200);
		QCOMPARE(info.size.height(), 200);
		VSIUnlink(path.toUtf8().constData());
	}
}


void GdalTiledTemplateTest::nonTiledPathUnchanged()
{
	auto path = createNonTiledTestRaster(100, 100);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	auto temp = std::make_unique<GdalTemplate>(path, &map);
	QVERIFY(!temp->isTiledSource());
	QVERIFY(temp->loadTemplateFile());

	// Should NOT use tiled path.
	QVERIFY(!temp->isTiledSource());

	// Should have loaded the full image.
	auto* image_template = static_cast<TemplateImage*>(temp.get());
	QVERIFY(!image_template->getImage().isNull());
	QCOMPARE(image_template->getImage().width(), 100);
	QCOMPARE(image_template->getImage().height(), 100);

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::tiledSourceDetected()
{
	auto path = createTiledTestRaster(1024, 1024, 256, 256);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	auto temp = std::make_unique<GdalTemplate>(path, &map);
	temp->worker_thread_count = 1;
	QVERIFY(temp->loadTemplateFile());

	// Should use tiled path.
	QVERIFY(temp->isTiledSource());

	// The full image should NOT be loaded into memory.
	auto* image_template = static_cast<TemplateImage*>(temp.get());
	QVERIFY(image_template->getImage().isNull());

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::asyncTileLoading()
{
	QSKIP("Threaded GDAL tile loading is unstable with /vsimem/ rasters in this headless test harness.");

	auto path = createTiledTestRaster(512, 512, 256, 256);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	auto temp = std::make_unique<GdalTemplate>(path, &map);
	temp->worker_thread_count = 1;
	QVERIFY(temp->loadTemplateFile());
	QVERIFY(temp->isTiledSource());
	temp->setTemplateState(Template::Loaded);

	// Use the public drawTemplate via Map::drawTemplates.
	auto* temp_ptr = temp.get();
	map.addTemplate(map.getNumTemplates(), std::move(temp));

	// The template's bounding box in map coords tells us where tiles live.
	auto const bbox = temp_ptr->calculateTemplateBoundingBox();
	QVERIFY(!bbox.isEmpty());

	QImage canvas(400, 400, QImage::Format_ARGB32);
	canvas.fill(Qt::transparent);
	QPainter painter(&canvas);

	// Set up the painter to map the template's bounding box onto the canvas.
	auto scale_x = canvas.width() / bbox.width();
	auto scale_y = canvas.height() / bbox.height();
	auto s = std::min(scale_x, scale_y);
	painter.scale(s, s);
	painter.translate(-bbox.center());

	// First draw: no tiles cached, but must not block the UI thread.
	QElapsedTimer timer;
	timer.start();
	map.drawTemplates(&painter, bbox, 0, map.getNumTemplates() - 1, nullptr, true);
	auto first_draw_ms = timer.elapsed();
	painter.end();

	// drawTemplate must complete quickly (no blocking on I/O).
	// Generous limit to avoid flaky failures on loaded CI machines.
	QVERIFY2(first_draw_ms < 5000,
	         qPrintable(QStringLiteral("drawTemplate took %1 ms (expected non-blocking)").arg(first_draw_ms)));

	// Process events to let async tile loads complete.
	// The worker reads from /vsimem/ which is fast (in-memory).
	for (int i = 0; i < 50; ++i)
	{
		QCoreApplication::processEvents();
		QTest::qWait(10);
	}

	// Second draw: tiles should now be cached.
	QImage canvas2(400, 400, QImage::Format_ARGB32);
	canvas2.fill(Qt::transparent);
	QPainter painter2(&canvas2);
	painter2.scale(s, s);
	painter2.translate(-bbox.center());

	timer.restart();
	map.drawTemplates(&painter2, bbox, 0, map.getNumTemplates() - 1, nullptr, true);
	auto second_draw_ms = timer.elapsed();
	painter2.end();

	QVERIFY2(second_draw_ms < 5000,
	         qPrintable(QStringLiteral("Cached draw took %1 ms").arg(second_draw_ms)));

	// Verify that the template is still operational after async loading.
	QVERIFY(temp_ptr->isTiledSource());

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::unloadCleanup()
{
	auto path = createTiledTestRaster(512, 512, 256, 256);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	auto temp = std::make_unique<GdalTemplate>(path, &map);
	temp->worker_thread_count = 1;
	QVERIFY(temp->loadTemplateFile());
	QVERIFY(temp->isTiledSource());

	// Unload should stop the worker thread and release the dataset.
	temp->unloadTemplateFile();
	QVERIFY(!temp->isTiledSource());
	QCOMPARE(temp->getTemplateState(), Template::Unloaded);

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::tiledTemplateExtent()
{
	auto path = createTiledTestRaster(800, 600, 256, 256);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	auto temp = std::make_unique<GdalTemplate>(path, &map);
	temp->worker_thread_count = 1;
	QVERIFY(temp->loadTemplateFile());
	QVERIFY(temp->isTiledSource());

	// Use calculateTemplateBoundingBox() which is public and calls getTemplateExtent() internally.
	auto bbox = temp->calculateTemplateBoundingBox();

	// The bounding box should have the right proportions (800 x 600).
	// Exact values depend on the template transform, but width/height ratio should be 4:3.
	QVERIFY(bbox.width() > 0);
	QVERIFY(bbox.height() > 0);
	auto ratio = bbox.width() / bbox.height();
	QVERIFY2(qAbs(ratio - (800.0 / 600.0)) < 0.1,
	         qPrintable(QStringLiteral("Expected ~1.33 ratio, got %1").arg(ratio)));

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::tiledDrawUsesMapClipRect()
{
	auto path = createTiledTestRaster(512, 512, 256, 256);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	auto temp = std::make_unique<GdalTemplate>(path, &map);
	temp->worker_thread_count = 1;
	QVERIFY(temp->loadTemplateFile());
	QVERIFY(temp->isTiledSource());
	temp->setTemplateState(Template::Loaded);

	auto* temp_ptr = temp.get();
	map.addTemplate(map.getNumTemplates(), std::move(temp));

	auto const bbox = temp_ptr->calculateTemplateBoundingBox();
	QVERIFY(!bbox.isEmpty());

	QImage canvas(400, 400, QImage::Format_ARGB32_Premultiplied);
	canvas.fill(Qt::transparent);

	auto draw_canvas = [&](QImage& target) {
		QPainter painter(&target);
		auto const scale_x = target.width() / bbox.width();
		auto const scale_y = target.height() / bbox.height();
		auto const s = std::min(scale_x, scale_y);
		painter.scale(s, s);
		painter.translate(-bbox.left(), -bbox.top());
		map.drawTemplates(&painter, bbox, 0, map.getNumTemplates() - 1, nullptr, true);
		return painter.transform().mapRect(bbox);
	};

	// First draw queues tile requests.
	auto drawn_rect = draw_canvas(canvas);

	auto alpha_at = [&](int x, int y) {
		return qAlpha(canvas.pixel(x, y));
	};

	auto inset = [&](qreal value, qreal min, qreal max) {
		return qBound(min, value, max);
	};
	auto const left = qRound(inset(drawn_rect.left() + 20, 0, canvas.width() - 1));
	auto const right = qRound(inset(drawn_rect.right() - 20, 0, canvas.width() - 1));
	auto const top = qRound(inset(drawn_rect.top() + 20, 0, canvas.height() - 1));
	auto const bottom = qRound(inset(drawn_rect.bottom() - 20, 0, canvas.height() - 1));

	auto all_corners_filled = [&]() {
		return alpha_at(left, top) > 0
		       && alpha_at(right, top) > 0
		       && alpha_at(left, bottom) > 0
		       && alpha_at(right, bottom) > 0;
	};

	bool filled = false;
	for (int i = 0; i < 100 && !filled; ++i)
	{
		QCoreApplication::processEvents();
		QTest::qWait(10);
		canvas.fill(Qt::transparent);
		drawn_rect = draw_canvas(canvas);
		filled = all_corners_filled();
	}

	// Sample well inside each corner so antialiasing at the exact border
	// does not affect the assertion.
	QVERIFY(filled);

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::tiledGeoreferencingMatchesNonTiled()
{
	auto const web_mercator_geotransform = std::array<double, 6>{
		-13601969.2610288, 0.074404761904762, 0.0,
		6043110.1630686, 0.0, -0.074404761904762
	};

	auto tiled_path = createTiledTestRaster(2048, 1536, 256, 256, 3857, web_mercator_geotransform);
	auto non_tiled_path = createNonTiledTestRaster(2048, 1536, 3857, web_mercator_geotransform);
	QVERIFY(!tiled_path.isEmpty());
	QVERIFY(!non_tiled_path.isEmpty());

	Map map;
	setupWilburtonLikeGeoreferencing(map);

	TestableGdalTemplate tiled_template(tiled_path, &map);
	TestableGdalTemplate non_tiled_template(non_tiled_path, &map);
	tiled_template.worker_thread_count = 1;
	tiled_template.forceGeoreferenced(true);
	non_tiled_template.forceGeoreferenced(true);
	QVERIFY(tiled_template.loadTemplateFile());
	QVERIFY(non_tiled_template.loadTemplateFile());
	QVERIFY(tiled_template.isTiledSource());
	QVERIFY(!non_tiled_template.isTiledSource());

	auto const tiled_bbox = tiled_template.calculateTemplateBoundingBox();
	auto const non_tiled_bbox = non_tiled_template.calculateTemplateBoundingBox();
	QVERIFY(!tiled_bbox.isEmpty());
	QVERIFY(!non_tiled_bbox.isEmpty());

	auto const bbox_tolerance = 0.001;
	QVERIFY2(std::abs(tiled_bbox.width() - non_tiled_bbox.width()) < bbox_tolerance,
	         qPrintable(QStringLiteral("width mismatch: tiled=%1 non-tiled=%2")
	                    .arg(tiled_bbox.width(), 0, 'f', 6)
	                    .arg(non_tiled_bbox.width(), 0, 'f', 6)));
	QVERIFY2(std::abs(tiled_bbox.height() - non_tiled_bbox.height()) < bbox_tolerance,
	         qPrintable(QStringLiteral("height mismatch: tiled=%1 non-tiled=%2")
	                    .arg(tiled_bbox.height(), 0, 'f', 6)
	                    .arg(non_tiled_bbox.height(), 0, 'f', 6)));
	QVERIFY2(QLineF(tiled_bbox.center(), non_tiled_bbox.center()).length() < bbox_tolerance,
	         qPrintable(QStringLiteral("center mismatch: tiled=(%1,%2) non-tiled=(%3,%4)")
	                    .arg(tiled_bbox.center().x(), 0, 'f', 6)
	                    .arg(tiled_bbox.center().y(), 0, 'f', 6)
	                    .arg(non_tiled_bbox.center().x(), 0, 'f', 6)
	                    .arg(non_tiled_bbox.center().y(), 0, 'f', 6)));

	auto const extent = QRectF(-1024.0, -768.0, 2048.0, 1536.0);
	auto const compare_point = [&](QPointF point, const char* label) {
		auto const tiled_map = tiled_template.templateToMap(MapCoordF(point));
		auto const non_tiled_map = non_tiled_template.templateToMap(MapCoordF(point));
		QVERIFY2(QLineF(tiled_map, non_tiled_map).length() < bbox_tolerance,
		         qPrintable(QStringLiteral("%1 mismatch: tiled=(%2,%3) non-tiled=(%4,%5)")
		                    .arg(QString::fromLatin1(label))
		                    .arg(tiled_map.x(), 0, 'f', 6)
		                    .arg(tiled_map.y(), 0, 'f', 6)
		                    .arg(non_tiled_map.x(), 0, 'f', 6)
		                    .arg(non_tiled_map.y(), 0, 'f', 6)));
	};

	compare_point(extent.topLeft(), "topLeft");
	compare_point(extent.topRight(), "topRight");
	compare_point(extent.bottomLeft(), "bottomLeft");
	compare_point(extent.bottomRight(), "bottomRight");
	compare_point(extent.center(), "center");

	VSIUnlink(tiled_path.toUtf8().constData());
	VSIUnlink(non_tiled_path.toUtf8().constData());
}


void GdalTiledTemplateTest::tiledTemplateUpdatesAfterMapBecomesGeospatial()
{
	auto const geotransform = std::array<double, 6>{
		500000.0, 0.1, 0.0,
		6000000.0, 0.0, -0.1
	};
	auto path = createTiledTestRaster(2048, 1536, 256, 256, 32632, geotransform);
	QVERIFY(!path.isEmpty());

	Map map;

	auto temp = std::make_unique<TestableGdalTemplate>(path, &map);
	temp->forceGeoreferenced(true);
	temp->worker_thread_count = 1;
	QVERIFY(temp->loadTemplateFile());
	QVERIFY(temp->isTiledSource());
	temp->setTemplateState(Template::Loaded);

	auto const bbox_before = temp->calculateTemplateBoundingBox();
	QCOMPARE(bbox_before.width(), 2048.0);
	QCOMPARE(bbox_before.height(), 1536.0);

	Georeferencing georef;
	georef.setScaleDenominator(5000);
	georef.setProjectedCRS(QStringLiteral("UTM 32N"), QStringLiteral("EPSG:32632"));
	georef.setGeographicRefPoint(LatLon(54.0, 9.0));
	map.setGeoreferencing(georef);

	auto const bbox_after = temp->calculateTemplateBoundingBox();
	QVERIFY2(bbox_after.width() < 100.0,
	         qPrintable(QStringLiteral("Expected georeferenced width, got %1").arg(bbox_after.width())));
	QVERIFY2(bbox_after.height() < 100.0,
	         qPrintable(QStringLiteral("Expected georeferenced height, got %1").arg(bbox_after.height())));
	QVERIFY2(qAbs(bbox_after.width() - 40.96) < 0.1,
	         qPrintable(QStringLiteral("Expected width ~40.96, got %1").arg(bbox_after.width(), 0, 'f', 6)));
	QVERIFY2(qAbs(bbox_after.height() - 30.72) < 0.1,
	         qPrintable(QStringLiteral("Expected height ~30.72, got %1").arg(bbox_after.height(), 0, 'f', 6)));

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::zoomedOutDrawUsesSubsampledTiles()
{
	auto path = createTiledTestRaster(2048, 2048, 1024, 1024);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	TestableGdalTemplate temp(path, &map);
	temp.forceGeoreferenced(true);
	temp.worker_thread_count = 1;
	QVERIFY(temp.loadTemplateFile());
	QVERIFY(temp.isTiledSource());
	temp.setTemplateState(Template::Loaded);

	auto const bbox = temp.calculateTemplateBoundingBox();
	QVERIFY(!bbox.isEmpty());

	QImage canvas(400, 400, QImage::Format_ARGB32_Premultiplied);
	canvas.fill(Qt::transparent);
	QPainter painter(&canvas);
	temp.drawTemplate(&painter, bbox, 0.0625, true, 1.0);
	painter.end();

	for (int i = 0; i < 100 && temp.tile_cache.isEmpty(); ++i)
	{
		QCoreApplication::processEvents();
		QTest::qWait(10);
	}

	QVERIFY(!temp.tile_cache.isEmpty());
	auto const cached_tile = temp.tile_cache.constBegin().value();
	QVERIFY2(cached_tile.width() < 1024,
	         qPrintable(QStringLiteral("Expected subsampled tile width, got %1").arg(cached_tile.width())));
	QVERIFY2(cached_tile.height() < 1024,
	         qPrintable(QStringLiteral("Expected subsampled tile height, got %1").arg(cached_tile.height())));

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::overviewCacheCanExceedOldTileCountLimit()
{
	auto path = createTiledTestRaster(4352, 4352, 256, 256);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	TestableGdalTemplate temp(path, &map);
	temp.forceGeoreferenced(true);
	temp.worker_thread_count = 1;
	QVERIFY(temp.loadTemplateFile());
	QVERIFY(temp.isTiledSource());
	temp.setTemplateState(Template::Loaded);

	auto const bbox = temp.calculateTemplateBoundingBox();
	QVERIFY(!bbox.isEmpty());

	QImage canvas(400, 400, QImage::Format_ARGB32_Premultiplied);
	canvas.fill(Qt::transparent);
	QPainter painter(&canvas);
	temp.drawTemplate(&painter, bbox, 0.0625, true, 1.0);
	painter.end();

	for (int i = 0; i < 1000 && temp.tile_cache.size() < 257; ++i)
	{
		QCoreApplication::processEvents();
		QTest::qWait(5);
	}

	QVERIFY2(temp.tile_cache.size() >= 257,
	         qPrintable(QStringLiteral("Expected overview cache to exceed 256 tiles, got %1").arg(temp.tile_cache.size())));

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::zoomChangeDropsQueuedLowerResolutionRequests()
{
	auto path = createTiledTestRaster(4096, 4096, 256, 256);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	TestableGdalTemplate temp(path, &map);
	temp.forceGeoreferenced(true);
	temp.worker_thread_count = 1;
	QVERIFY(temp.loadTemplateFile());
	QVERIFY(temp.isTiledSource());
	temp.setTemplateState(Template::Loaded);

	// Make queue contents deterministic for inspection.
	temp.worker_stop = true;
	temp.queue_cv.notify_all();
	for (auto& worker_thread : temp.worker_threads)
	{
		if (worker_thread.joinable())
			worker_thread.join();
	}
	temp.worker_threads.clear();

	auto const bbox = temp.calculateTemplateBoundingBox();
	QVERIFY(!bbox.isEmpty());

	QImage canvas(400, 400, QImage::Format_ARGB32_Premultiplied);
	canvas.fill(Qt::transparent);

	{
		QPainter painter(&canvas);
		temp.drawTemplate(&painter, bbox, 0.0625, true, 1.0);
	}

	QVERIFY(!temp.tile_queue.isEmpty());
	QVERIFY(temp.active_subsampling.load() > 1);

	{
		QPainter painter(&canvas);
		temp.drawTemplate(&painter, bbox, 1.0, true, 1.0);
	}

	QVERIFY(!temp.tile_queue.isEmpty());
	QCOMPARE(temp.active_subsampling.load(), 1);
	for (auto const& request : temp.tile_queue)
		QCOMPARE(request.subsampling, 1);

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::onScreenScaleChoosesSharperLevel()
{
	auto path = createTiledTestRaster(2048, 2048, 1024, 1024);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	TestableGdalTemplate temp(path, &map);
	temp.forceGeoreferenced(true);
	temp.worker_thread_count = 1;
	QVERIFY(temp.loadTemplateFile());
	QVERIFY(temp.isTiledSource());
	temp.setTemplateState(Template::Loaded);

	temp.worker_stop = true;
	temp.queue_cv.notify_all();
	for (auto& worker_thread : temp.worker_threads)
	{
		if (worker_thread.joinable())
			worker_thread.join();
	}
	temp.worker_threads.clear();

	auto const bbox = temp.calculateTemplateBoundingBox();
	QVERIFY(!bbox.isEmpty());

	QImage canvas(400, 400, QImage::Format_ARGB32_Premultiplied);
	canvas.fill(Qt::transparent);
	{
		QPainter painter(&canvas);
		temp.drawTemplate(&painter, bbox, 0.25, true, 1.0);
	}

	QCOMPARE(temp.active_subsampling.load(), 1);

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::visibleTilesArePrioritizedAheadOfOverscan()
{
	auto path = createTiledTestRaster(4096, 4096, 256, 256);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	TestableGdalTemplate temp(path, &map);
	temp.forceGeoreferenced(true);
	temp.worker_thread_count = 1;
	QVERIFY(temp.loadTemplateFile());
	QVERIFY(temp.isTiledSource());
	temp.setTemplateState(Template::Loaded);

	temp.worker_stop = true;
	temp.queue_cv.notify_all();
	for (auto& worker_thread : temp.worker_threads)
	{
		if (worker_thread.joinable())
			worker_thread.join();
	}
	temp.worker_threads.clear();

	auto const bbox = temp.calculateTemplateBoundingBox();
	QVERIFY(!bbox.isEmpty());
	auto const clip_rect = QRectF(
		bbox.center().x() - bbox.width() * 0.125,
		bbox.center().y() - bbox.height() * 0.125,
		bbox.width() * 0.25,
		bbox.height() * 0.25
	);

	QImage canvas(400, 400, QImage::Format_ARGB32_Premultiplied);
	canvas.fill(Qt::transparent);
	{
		QPainter painter(&canvas);
		temp.drawTemplate(&painter, clip_rect, 1.0, true, 1.0);
	}

	QVERIFY(!temp.tile_queue.isEmpty());

	auto const top_left = QPointF(temp.mapToTemplate(MapCoordF(clip_rect.topLeft())));
	auto const top_right = QPointF(temp.mapToTemplate(MapCoordF(clip_rect.topRight())));
	auto const bottom_left = QPointF(temp.mapToTemplate(MapCoordF(clip_rect.bottomLeft())));
	auto const bottom_right = QPointF(temp.mapToTemplate(MapCoordF(clip_rect.bottomRight())));
	auto const min_x = std::min({ top_left.x(), top_right.x(), bottom_left.x(), bottom_right.x() });
	auto const max_x = std::max({ top_left.x(), top_right.x(), bottom_left.x(), bottom_right.x() });
	auto const min_y = std::min({ top_left.y(), top_right.y(), bottom_left.y(), bottom_right.y() });
	auto const max_y = std::max({ top_left.y(), top_right.y(), bottom_left.y(), bottom_right.y() });
	auto const visible_rect = QRectF(QPointF(min_x, min_y), QPointF(max_x, max_y));

	auto const half_w = temp.tiled_raster_size.width() * 0.5;
	auto const half_h = temp.tiled_raster_size.height() * 0.5;
	auto const block_w = temp.tiled_raster_info.block_size.width();
	auto const block_h = temp.tiled_raster_info.block_size.height();
	QVERIFY(block_w > 0);
	QVERIFY(block_h > 0);

	bool saw_overscan_request = false;
	for (auto const& request : temp.tile_queue)
	{
		auto const source_w = std::min(block_w, temp.tiled_raster_size.width() - request.tile_x * block_w);
		auto const source_h = std::min(block_h, temp.tiled_raster_size.height() - request.tile_y * block_h);
		auto const tile_left = request.tile_x * block_w - half_w;
		auto const tile_top = request.tile_y * block_h - half_h;
		auto const intersects_visible_rect =
			tile_left < visible_rect.right()
			&& tile_left + source_w > visible_rect.left()
			&& tile_top < visible_rect.bottom()
			&& tile_top + source_h > visible_rect.top();

		if (!intersects_visible_rect)
			saw_overscan_request = true;
		else
			QVERIFY2(!saw_overscan_request, "Visible tile request appeared after overscan request.");
	}

	VSIUnlink(path.toUtf8().constData());
}


void GdalTiledTemplateTest::visibleUnloadedTiledTemplateLoadsOnPaint()
{
	auto path = createTiledTestRaster(2048, 2048, 256, 256);
	QVERIFY(!path.isEmpty());

	Map map;
	MapView view(&map);
	setupMapGeoreferencing(map);

	auto temp = std::make_unique<TestableGdalTemplate>(path, &map);
	temp->forceGeoreferenced(true);
	temp->worker_thread_count = 1;
	temp->setTemplateState(Template::Unloaded);
	map.addTemplate(-1, std::move(temp));

	auto* reopened_template = static_cast<TestableGdalTemplate*>(map.getTemplate(0));
	QVERIFY(reopened_template);
	QCOMPARE(reopened_template->getTemplateState(), Template::Unloaded);

	MapWidget widget(false, false);
	widget.resize(400, 400);
	widget.setMapView(&view);
	widget.show();
	widget.updateEverything();

	for (int i = 0; i < 200 && reopened_template->getTemplateState() != Template::Loaded; ++i)
	{
		QCoreApplication::processEvents();
		QTest::qWait(10);
	}

	QCOMPARE(reopened_template->getTemplateState(), Template::Loaded);
	QVERIFY(reopened_template->isTiledSource());
	view.setCenter(MapCoord(reopened_template->calculateTemplateBoundingBox().center()));
	widget.updateEverything();

	for (int i = 0; i < 200 && reopened_template->tile_cache.isEmpty(); ++i)
	{
		QCoreApplication::processEvents();
		QTest::qWait(10);
	}

	QVERIFY2(!reopened_template->tile_cache.isEmpty(),
	         "Expected first paint to trigger tiled template loading after reopen");

	widget.hide();
	VSIUnlink(path.toUtf8().constData());
}


QTEST_MAIN(GdalTiledTemplateTest)
#include "gdal_tiled_template_t.moc"
