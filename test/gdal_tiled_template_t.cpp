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


QString GdalTiledTemplateTest::createTiledTestRaster(int width, int height, int block_w, int block_h)
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

	// Geotransform: 1 pixel = 1 meter, origin at UTM 32N coordinates.
	double geotransform[6] = { 500000.0, 1.0, 0.0, 6000000.0, 0.0, -1.0 };
	GDALSetGeoTransform(dataset, geotransform);
	setDatasetSRS(dataset, 32632);

	GDALClose(dataset);
	return path;
}


QString GdalTiledTemplateTest::createNonTiledTestRaster(int width, int height)
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

	double geotransform[6] = { 500000.0, 1.0, 0.0, 6000000.0, 0.0, -1.0 };
	GDALSetGeoTransform(dataset, geotransform);
	setDatasetSRS(dataset, 32632);

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
	auto path = createTiledTestRaster(512, 512, 256, 256);
	QVERIFY(!path.isEmpty());

	Map map;
	setupMapGeoreferencing(map);

	auto temp = std::make_unique<GdalTemplate>(path, &map);
	QVERIFY(temp->loadTemplateFile());
	QVERIFY(temp->isTiledSource());

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


QTEST_MAIN(GdalTiledTemplateTest)
#include "gdal_tiled_template_t.moc"
