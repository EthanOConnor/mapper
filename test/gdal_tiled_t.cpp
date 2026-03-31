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
#include <QFile>
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
#include "gdal/online_imagery_template_builder.h"
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

QString writeTextFile(const QString& path, const QByteArray& contents)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
		return {};
	if (file.write(contents) != contents.size())
		return {};
	return path;
}

QString createTmsXml(const QString& path)
{
	return writeTextFile(
		path,
		R"(<?xml version="1.0" encoding="UTF-8"?>
<GDAL_WMS>
  <Service name="TMS">
    <ServerUrl>https://example.test/${z}/${x}/${y}.png</ServerUrl>
  </Service>
  <DataWindow>
    <UpperLeftX>-13618288.0</UpperLeftX>
    <UpperLeftY>6050654.0</UpperLeftY>
    <LowerRightX>-13617776.0</LowerRightX>
    <LowerRightY>6050142.0</LowerRightY>
    <SizeX>512</SizeX>
    <SizeY>512</SizeY>
    <TileLevel>18</TileLevel>
    <TileX>84192</TileX>
    <TileY>183072</TileY>
    <YOrigin>top</YOrigin>
  </DataWindow>
  <Projection>EPSG:3857</Projection>
  <BlockSizeX>256</BlockSizeX>
  <BlockSizeY>256</BlockSizeY>
  <BandsCount>3</BandsCount>
  <ZeroBlockHttpCodes>404</ZeroBlockHttpCodes>
  <Cache />
</GDAL_WMS>
)");
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

	void tmsTileOriginParsingTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto const tms_xml_path = createTmsXml(dir.filePath(QStringLiteral("tms.xml")));
		QVERIFY(!tms_xml_path.isEmpty());

		QPoint origin_tile;
		QVERIFY(GdalTemplate::readTmsTileOrigin(tms_xml_path, &origin_tile));
		QCOMPARE(origin_tile, QPoint(84192, 183072));

		auto const wms_xml_path = writeTextFile(
			dir.filePath(QStringLiteral("wms.xml")),
			R"(<?xml version="1.0" encoding="UTF-8"?>
<GDAL_WMS>
  <Service name="WMS">
    <ServerUrl>https://example.test/wms</ServerUrl>
  </Service>
  <DataWindow>
    <TileX>10</TileX>
    <TileY>20</TileY>
  </DataWindow>
</GDAL_WMS>
)");
		QVERIFY(!wms_xml_path.isEmpty());
		QVERIFY(!GdalTemplate::readTmsTileOrigin(wms_xml_path, &origin_tile));
	}

	void onlineImageryClassificationTest()
	{
		auto arcgis_template = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"));
		QCOMPARE(arcgis_template.source.kind, OnlineImagerySource::Kind::ArcGisTiledMapServer);
		QCOMPARE(arcgis_template.source.normalized_url,
		         QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer"));

		auto xyz = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"));
		QCOMPARE(xyz.source.kind, OnlineImagerySource::Kind::XyzTiles);
		QCOMPARE(xyz.source.normalized_url,
		         QStringLiteral("https://tile.openstreetmap.org/${z}/${x}/${y}.png"));
		QCOMPARE(xyz.source.tile_size, QSize(256, 256));
		QCOMPARE(xyz.source.max_tile_level, 19);

		auto arcgis = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer/tile/5/10/12"));
		QCOMPARE(arcgis.source.kind, OnlineImagerySource::Kind::ArcGisTiledMapServer);
		QCOMPARE(arcgis.source.normalized_url,
		         QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer"));

		auto arcgis_root = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer"));
		QCOMPARE(arcgis_root.source.kind, OnlineImagerySource::Kind::ArcGisTiledMapServer);
		QCOMPARE(arcgis_root.source.normalized_url,
		         QStringLiteral("https://server.example.test/ArcGIS/rest/services/World_Imagery/MapServer"));
		QCOMPARE(arcgis_root.source.tile_size, QSize(256, 256));
		QCOMPARE(arcgis_root.source.max_tile_level, 20);

		auto unsupported = OnlineImageryTemplateBuilder::classifyUrl(
			QStringLiteral("https://tiles.example.test/{s}/{z}/{x}/{y}.png"));
		QVERIFY(!unsupported.error.isEmpty());
	}

	void onlineImageryCoordinateMathTest()
	{
		auto const origin = OnlineImageryTemplateBuilder::latLonToWebMercator(0.0, 0.0);
		QVERIFY(qAbs(origin.x()) < 0.001);
		QVERIFY(qAbs(origin.y()) < 0.001);

		auto const east = OnlineImageryTemplateBuilder::latLonToWebMercator(0.0, 180.0);
		QVERIFY(qAbs(east.x() - 20037508.342789244) < 0.001);
		QVERIFY(qAbs(east.y()) < 0.001);

		auto const crop = OnlineImageryTemplateBuilder::snapToTileGrid(
			QRectF(-10018754.171394622, -10018754.171394622, 20037508.342789244, 20037508.342789244),
			2,
			256);
		QCOMPARE(crop.tile_x_min, 0);
		QCOMPARE(crop.tile_x_max, 3);
		QCOMPARE(crop.tile_y_min, 0);
		QCOMPARE(crop.tile_y_max, 3);
		QCOMPARE(crop.pixel_width, 1024);
		QCOMPARE(crop.pixel_height, 1024);
		QVERIFY(qAbs(crop.west + 20037508.342789244) < 0.001);
		QVERIFY(qAbs(crop.north - 20037508.342789244) < 0.001);
		QVERIFY(qAbs(crop.east - 20037508.342789244) < 0.001);
		QVERIFY(qAbs(crop.south + 20037508.342789244) < 0.001);

		auto const detailed_crop = OnlineImageryTemplateBuilder::snapToTileGrid(
			QRectF(-13602122.057404, 6039136.730755, 4891.969811, 4891.969810),
			19,
			256);
		QCOMPARE(detailed_crop.tile_x_min % 32, 0);
		QCOMPARE(detailed_crop.tile_y_min % 32, 0);
		QCOMPARE(detailed_crop.tile_x_max % 32, 31);
		QCOMPARE(detailed_crop.tile_y_max % 32, 31);
	}

	void onlineImageryOutputFileNameTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		OnlineImagerySource source;
		source.kind = OnlineImagerySource::Kind::XyzTiles;
		source.display_name = QStringLiteral("openstreetmap");
		source.normalized_url = QStringLiteral("https://tile.openstreetmap.org/${z}/${x}/${y}.png");

		auto named_path = OnlineImageryTemplateBuilder::outputFileName(
			dir.filePath(QStringLiteral("wilburton.omap")),
			source,
			QStringLiteral("Wilburton Hill"));
		QCOMPARE(QFileInfo(named_path).dir().absolutePath(), dir.path());
		QVERIFY(QFileInfo(named_path).fileName().startsWith(QStringLiteral("wilburton_hill_wilburton_online_")));
		QVERIFY(named_path.endsWith(QStringLiteral(".xml")));

		auto default_path = OnlineImageryTemplateBuilder::outputFileName(
			dir.filePath(QStringLiteral("wilburton.omap")),
			source,
			QString{});
		QVERIFY(QFileInfo(default_path).fileName().startsWith(QStringLiteral("openstreetmap_wilburton_online_")));
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
		QCOMPARE(GdalTemplate::chooseTileSubsampling(0.75, QSize(64, 64)), 1);
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
		QCOMPARE(GdalTemplate::sourceRectWithinCachedTile(QRect(64, 0, 64, 64),
		                                                  QRect(0, 0, 128, 128),
		                                                  QSize(64, 64)),
		         QRectF(32.0, 0.0, 32.0, 32.0));
		QCOMPARE(GdalTemplate::sourceRectWithinCachedTile(QRect(128, 0, 2, 64),
		                                                  QRect(0, 0, 130, 128),
		                                                  QSize(65, 64)),
		         QRectF(64.0, 0.0, 1.0, 32.0));

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

		QCOMPARE(temp.chooseTiledSubsampling(4.0), 1);
		temp.has_tiled_origin_tile = true;
		temp.tiled_origin_tile = QPoint(6, 12);
		QCOMPARE(temp.chooseTiledSubsampling(0.125), 2);
		temp.tiled_origin_tile = QPoint(84192, 183072);
		QCOMPARE(temp.chooseTiledSubsampling(0.03), 32);
		QCOMPARE(temp.chooseTiledSubsampling(0.01), 32);

		auto const coarse_key = GdalTemplate::tileKey(0, 0, 2);
		temp.tile_cache.insert(coarse_key, GdalTemplate::CachedTileEntry{ QImage(64, 64, QImage::Format_ARGB32_Premultiplied) });

		QRectF fallback_source_rect;
		auto const* fallback = temp.findBestCachedTile(1, 0, 1, &fallback_source_rect);
		QVERIFY(fallback);
		QCOMPARE(fallback->size(), QSize(64, 64));
		QCOMPARE(fallback_source_rect, QRectF(32.0, 0.0, 32.0, 32.0));

		temp.tiled_dataset = nullptr;
	}

	void workerCountForSourceTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("local.tif")),
		                                     QSize(128, 128),
		                                     QSize(64, 64),
		                                     true);
		QVERIFY(!tiled_path.isEmpty());

		Map map;

		GdalTemplate local_template(tiled_path, &map);
		local_template.tiled_dataset = GDALOpen(tiled_path.toUtf8(), GA_ReadOnly);
		QVERIFY(local_template.tiled_dataset);
		QCOMPARE(local_template.workerCountForSource(), 1);
		QCOMPARE(GdalTemplate::workerCountForDriverName("GTiff"), 1);
		QCOMPARE(GdalTemplate::workerCountForDriverName("WMS"), 4);
		QCOMPARE(GdalTemplate::workerCountForDriverName(nullptr), 1);
		local_template.shutdownTiledSource();
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
		QCOMPARE(gdal_template->workerCountForSource(), 1);
		QCOMPARE(gdal_template->worker_datasets.size(), std::size_t(1));
		QCOMPARE(gdal_template->worker_threads.size(), std::size_t(1));

		std::unique_ptr<Template> duplicate{gdal_template->duplicate()};
		QVERIFY(duplicate);
		QCOMPARE(duplicate->getTemplateType(), "GdalTemplate");
		QCOMPARE(duplicate->getTemplateState(), Template::Loaded);
		auto* duplicate_gdal = dynamic_cast<GdalTemplate*>(duplicate.get());
		QVERIFY(duplicate_gdal);
		QVERIFY(duplicate_gdal->isTiledSource());
		QVERIFY(duplicate_gdal->isTemplateGeoreferenced());
		QCOMPARE(duplicate_gdal->workerCountForSource(), 1);
		QCOMPARE(duplicate_gdal->worker_datasets.size(), std::size_t(1));
		QCOMPARE(duplicate_gdal->worker_threads.size(), std::size_t(1));
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

	void nonGeoreferencedTiledRasterFallsBackToFullImageTest()
	{
		QTemporaryDir dir;
		QVERIFY(dir.isValid());

		auto tiled_path = createTiledGeoTiff(dir.filePath(QStringLiteral("non-georef.tif")),
		                                     QSize(128, 128),
		                                     QSize(64, 64),
		                                     false);
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
		QVERIFY(!gdal_template->isTiledSource());
		QVERIFY(!gdal_template->isTemplateGeoreferenced());
		QCOMPARE(gdal_template->getTemplateExtent(), QRectF(-64.0, -64.0, 128.0, 128.0));
	}
};

}  // namespace OpenOrienteering

using OpenOrienteering::GdalTiledTest;

QTEST_MAIN(GdalTiledTest)
#include "gdal_tiled_t.moc"
