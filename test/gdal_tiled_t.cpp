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

#include <memory>
#include <vector>

#include <QtTest>
#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <ogr_srs_api.h>

#include "test_config.h"

#include "global.h"
#include "core/map.h"
#include "gdal/gdal_image_reader.h"
#include "gdal/gdal_manager.h"
#include "gdal/gdal_template.h"
#include "templates/template.h"

namespace OpenOrienteering {

namespace {

QString createTiledGeoTiff(const QString& path,
                           const QSize& raster_size,
                           const QSize& block_size,
                           bool georeferenced)
{
	auto* driver = GDALGetDriverByName("GTiff");
	if (!driver)
		return {};

	char** options = nullptr;
	if (!block_size.isEmpty())
	{
		options = CSLSetNameValue(options, "TILED", "YES");
		options = CSLSetNameValue(options, "BLOCKXSIZE", QByteArray::number(block_size.width()).constData());
		options = CSLSetNameValue(options, "BLOCKYSIZE", QByteArray::number(block_size.height()).constData());
	}

	auto* dataset = GDALCreate(driver,
	                           path.toUtf8().constData(),
	                           raster_size.width(),
	                           raster_size.height(),
	                           1,
	                           GDT_Byte,
	                           options);
	CSLDestroy(options);
	if (!dataset)
		return {};

	std::vector<GByte> pixels(std::size_t(raster_size.width()) * raster_size.height());
	for (int y = 0; y < raster_size.height(); ++y)
	{
		for (int x = 0; x < raster_size.width(); ++x)
			pixels[std::size_t(y) * raster_size.width() + x] = GByte((x + y) % 256);
	}

	auto result = GDALRasterIO(GDALGetRasterBand(dataset, 1),
	                           GF_Write,
	                           0,
	                           0,
	                           raster_size.width(),
	                           raster_size.height(),
	                           pixels.data(),
	                           raster_size.width(),
	                           raster_size.height(),
	                           GDT_Byte,
	                           0,
	                           0);
	if (result < CE_Warning && georeferenced)
	{
		double geo_transform[6] = { 410000.0, 2.0, 0.0, 5300000.0, 0.0, -2.0 };
		result = GDALSetGeoTransform(dataset, geo_transform);
		if (result < CE_Warning)
		{
			auto* srs = OSRNewSpatialReference(nullptr);
			if (srs && OSRSetFromUserInput(srs, "EPSG:3857") == OGRERR_NONE)
			{
				char* wkt = nullptr;
				if (OSRExportToWkt(srs, &wkt) == OGRERR_NONE)
				{
					result = GDALSetProjection(dataset, wkt);
					CPLFree(wkt);
				}
			}
			OSRDestroySpatialReference(srs);
		}
	}

	GDALClose(dataset);
	return (result < CE_Warning) ? path : QString{};
}

}  // namespace


class GdalTiledTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase()
	{
		QCoreApplication::setOrganizationName(QString::fromLatin1("OpenOrienteering.org"));
		QCoreApplication::setApplicationName(QString::fromLatin1(metaObject()->className()));
		QVERIFY2(QDir::home().exists(), "The home dir must be writable in order to use QSettings.");

		doStaticInitializations();
		GdalManager();
		QDir::addSearchPath(QStringLiteral("testdata"),
		                    QDir(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR)).absoluteFilePath(QStringLiteral("data")));
	}

	void tiledRasterDetectionTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("tiled-64.tif")),
		                                     QSize(128, 128),
		                                     QSize(64, 64),
		                                     true);
		QVERIFY(!tiled_path.isEmpty());
		auto tiled_info = GdalImageReader(tiled_path).readRasterInfo();
		QCOMPARE(tiled_info.size, QSize(128, 128));
		QCOMPARE(tiled_info.block_size, QSize(64, 64));

		auto small_tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("tiled-32.tif")),
		                                           QSize(128, 128),
		                                           QSize(32, 32),
		                                           true);
		QVERIFY(!small_tiled_path.isEmpty());
		auto small_tiled_info = GdalImageReader(small_tiled_path).readRasterInfo();
		QCOMPARE(small_tiled_info.block_size, QSize());
	}

	void tiledCoreMathTest()
	{
		auto const normalized = GdalTemplate::tileKey(3, 4, 0);
		QCOMPARE(normalized.tile_x, 3);
		QCOMPARE(normalized.tile_y, 4);
		QCOMPARE(normalized.subsampling, 1);

		QSet<GdalTileKey> keys;
		keys.insert(normalized);
		QVERIFY(keys.contains(GdalTemplate::tileKey(3, 4, 1)));

		QCOMPARE(GdalTemplate::chooseTileSubsampling(4.0, QSize(64, 64)), 1);
		QCOMPARE(GdalTemplate::chooseTileSubsampling(0.5, QSize(64, 64)), 2);
		QCOMPARE(GdalTemplate::chooseTileSubsampling(0.25, QSize(64, 64)), 4);

		QCOMPARE(GdalTemplate::sourceRectForTile(QSize(130, 130), QSize(64, 64), 0, 0, 1),
		         QRect(0, 0, 64, 64));
		QCOMPARE(GdalTemplate::sourceRectForTile(QSize(130, 130), QSize(64, 64), 2, 2, 1),
		         QRect(128, 128, 2, 2));
		QCOMPARE(GdalTemplate::sourceRectForTile(QSize(130, 130), QSize(64, 64), 0, 0, 2),
		         QRect(0, 0, 128, 128));
		QCOMPARE(GdalTemplate::sourceRectForTile(QSize(130, 130), QSize(64, 64), 2, 2, 2),
		         QRect());

		Map map;
		GdalTemplate temp(QStringLiteral("dummy.tif"), &map);
		temp.tiled_dataset = reinterpret_cast<GDALDatasetH>(quintptr(1));
		temp.tiled_raster_size = QSize(256, 128);
		temp.tiled_raster_info.block_size = QSize(64, 64);

		auto const full_window = temp.tileWindowForMapRect(QRectF(-128.0, -64.0, 256.0, 128.0), 1);
		QCOMPARE(full_window.tile_x_min, 0);
		QCOMPARE(full_window.tile_y_min, 0);
		QCOMPARE(full_window.tile_x_max, 3);
		QCOMPARE(full_window.tile_y_max, 1);
		QCOMPARE(full_window.subsampling, 1);

		auto const edge_window = temp.tileWindowForMapRect(QRectF(96.0, -64.0, 64.0, 128.0), 2);
		QCOMPARE(edge_window.tile_x_min, 1);
		QCOMPARE(edge_window.tile_y_min, 0);
		QCOMPARE(edge_window.tile_x_max, 1);
		QCOMPARE(edge_window.tile_y_max, 0);
		QCOMPARE(edge_window.subsampling, 2);

		temp.tiled_dataset = nullptr;
	}

	void duplicateLoadedTiledTemplateTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("roundtrip.tif")),
		                                     QSize(128, 128),
		                                     QSize(64, 64),
		                                     true);
		QVERIFY(!tiled_path.isEmpty());

		Map map;
		auto temp = Template::templateForPath(tiled_path, &map);
		QVERIFY(temp);
		QCOMPARE(temp->getTemplateType(), "GdalTemplate");

		map.addTemplate(0, std::move(temp));
		auto* gdal_template = dynamic_cast<GdalTemplate*>(map.getTemplate(0));
		QVERIFY(gdal_template);
		gdal_template->setTemplateState(Template::Unloaded);
		QVERIFY(gdal_template->loadTemplateFile());
		QCOMPARE(gdal_template->getTemplateState(), Template::Loaded);
		QVERIFY(gdal_template->isTiledSource());
		QVERIFY(gdal_template->isTemplateGeoreferenced());

		std::unique_ptr<Template> duplicate{gdal_template->duplicate()};
		QVERIFY(duplicate);
		QCOMPARE(duplicate->getTemplateType(), "GdalTemplate");
		QCOMPARE(duplicate->getTemplateState(), Template::Loaded);
		auto* duplicate_gdal = dynamic_cast<GdalTemplate*>(duplicate.get());
		QVERIFY(duplicate_gdal);
		QVERIFY(duplicate_gdal->isTiledSource());
		QVERIFY(duplicate_gdal->isTemplateGeoreferenced());
	}

	void roundTripGeoreferencedTiledTemplateTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("roundtrip.tif")),
		                                     QSize(128, 128),
		                                     QSize(64, 64),
		                                     true);
		QVERIFY(!tiled_path.isEmpty());

		Map map;
		auto temp = Template::templateForPath(tiled_path, &map);
		QVERIFY(temp);
		QCOMPARE(temp->getTemplateType(), "GdalTemplate");

		map.addTemplate(0, std::move(temp));
		auto* gdal_template = dynamic_cast<GdalTemplate*>(map.getTemplate(0));
		QVERIFY(gdal_template);
		gdal_template->setTemplateState(Template::Unloaded);
		QVERIFY(gdal_template->loadTemplateFile());
		QCOMPARE(gdal_template->getTemplateState(), Template::Loaded);
		QVERIFY(gdal_template->isTiledSource());
		QVERIFY(gdal_template->isTemplateGeoreferenced());

		auto const original_crs = gdal_template->availableGeoreferencing().effective.crs_spec;
		QVERIFY(!original_crs.isEmpty());

		gdal_template->unloadTemplateFile();
		QCOMPARE(gdal_template->getTemplateState(), Template::Unloaded);

		QBuffer buffer;
		QVERIFY(buffer.open(QIODevice::ReadWrite));
		QXmlStreamWriter writer(&buffer);
		gdal_template->saveTemplateConfiguration(writer, true, nullptr);
		QVERIFY(buffer.seek(0));

		QXmlStreamReader reader(&buffer);
		QVERIFY(reader.readNextStartElement());
		bool open = false;
		Map reloaded_map;
		auto reloaded_template_holder = Template::loadTemplateConfiguration(reader, reloaded_map, open);
		QVERIFY(reloaded_template_holder);
		QVERIFY(open);

		auto* reloaded_gdal = dynamic_cast<GdalTemplate*>(reloaded_template_holder.get());
		QVERIFY(reloaded_gdal);
		QCOMPARE(reloaded_gdal->getTemplateType(), "GdalTemplate");
		QVERIFY(reloaded_gdal->isTemplateGeoreferenced());
		QCOMPARE(reloaded_gdal->getTemplateFilename(), QStringLiteral("roundtrip.tif"));
		QVERIFY(reloaded_gdal->loadTemplateFile());
		QCOMPARE(reloaded_gdal->getTemplateState(), Template::Loaded);
		QVERIFY(reloaded_gdal->isTiledSource());
		QCOMPARE(reloaded_gdal->availableGeoreferencing().effective.crs_spec, original_crs);
	}
};

}  // namespace OpenOrienteering

using OpenOrienteering::GdalTiledTest;

QTEST_MAIN(GdalTiledTest)
#include "gdal_tiled_t.moc"
