/*
 *    Copyright 2020-2021 Kai Pastor
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

#include "kmz_groundoverlay_export.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

#include <cpl_vsi.h>
#include <cpl_vsi_error.h>
// IWYU pragma: no_include <gdal.h>

#include <Qt>
#include <QtGlobal>
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QIODevice>
#include <QLatin1String>
#include <QLineF>
#include <QLockFile>
#include <QPainter>
#include <QPoint>
#include <QPointF>
#include <QProgressDialog>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTransform>

#include "mapper_config.h"
#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map.h"
#include "core/map_coord.h"
#include "core/map_printer.h"
#include "fileformats/file_format.h"
#include "gdal/gdal_file.h"
#include "util/util.h"

// IWYU pragma: no_forward_declare QRectF
// IWYU pragma: no_forward_declare QXmlStreamWriter


namespace OpenOrienteering {

namespace {

static constexpr const char* format = "jpg";
static constexpr int quality        = 75;


QPointF toLonLat(const LatLon& latlon) noexcept
{
	return QPointF(latlon.longitude(), latlon.latitude());
}

LatLon fromLonLat(const QPointF& p) noexcept
{
	return LatLon(p.y(), p.x());
}


QRectF boundingBox(const QPointF& p0, const QPointF& p1, const QPointF& p2, const QPointF& p3) noexcept
{
	auto result = QRectF{p0.x(), p0.y(), 0, 0};
	rectInclude(result, p1);
	rectInclude(result, p2);
	rectInclude(result, p3);
	return result;
}


QRectF boundingBoxLonLat(const Georeferencing& georef, const MapCoordF& p0, const MapCoordF& p1, const MapCoordF& p2, const MapCoordF& p3)
{
	return boundingBox(
	            toLonLat(georef.toGeographicCoords(p0)),
	            toLonLat(georef.toGeographicCoords(p1)),
	            toLonLat(georef.toGeographicCoords(p2)),
	            toLonLat(georef.toGeographicCoords(p3))
	);
}

QRectF boundingBoxLonLat(const Georeferencing& georef, const QRectF& extent_map_coord)
{
	return boundingBoxLonLat(
	            georef,
	            MapCoordF(extent_map_coord.topLeft()),
	            MapCoordF(extent_map_coord.topRight()),
	            MapCoordF(extent_map_coord.bottomRight()),
	            MapCoordF(extent_map_coord.bottomLeft())
	);
}


QRectF boundingBoxMap(const Georeferencing& georef, const QPointF& p0, const QPointF& p1, const QPointF& p2, const QPointF& p3)
{
	return boundingBox(
	            georef.toMapCoordF(fromLonLat(p0)),
	            georef.toMapCoordF(fromLonLat(p1)),
	            georef.toMapCoordF(fromLonLat(p2)),
	            georef.toMapCoordF(fromLonLat(p3))
	);
}

QRectF boundingBoxMap(const Georeferencing& georef, const QRectF& extent_lonlat)
{
	return boundingBoxMap(
	            georef,
	            extent_lonlat.topLeft(),
	            extent_lonlat.topRight(),
	            extent_lonlat.bottomRight(),
	            extent_lonlat.bottomLeft()
	);
}

QByteArray directAssetPrefix(
	const QByteArray& output_filepath)
{
	return QByteArray(".mapper-kml-")
	       + QCryptographicHash::hash(
		         output_filepath,
		         QCryptographicHash::Sha256)
		         .toHex()
		         .left(16)
	       + QByteArray("-assets-");
}

QByteArray stagingFilePrefix(
	const QByteArray& output_filepath,
	bool kmz)
{
	return QByteArray(".mapper-")
	       + (kmz ? QByteArray("kmz-") : QByteArray("kml-"))
	       + QCryptographicHash::hash(
		         output_filepath,
		         QCryptographicHash::Sha256)
		         .toHex()
	       + QByteArray("-stage-");
}


}  // namespace



// ### KmzGroudOverlayExport ###

// static
KmzGroundOverlayExport::Metrics KmzGroundOverlayExport::makeMetrics(const QSizeF& area_size, qreal const resolution_dpi, int const tile_width_px)
{
	auto const units_per_mm = resolution_dpi / 25.4;
	auto tile_size_px = QSize{ tile_width_px, tile_width_px };
	if (tile_width_px <= 0)
	{
		tile_size_px.setWidth(std::ceil(area_size.width() * units_per_mm));
		tile_size_px.setHeight(std::ceil(area_size.height() * units_per_mm));
	}
	return Metrics { tile_size_px, resolution_dpi, units_per_mm, QSizeF(tile_size_px) / units_per_mm };
}


KmzGroundOverlayExport::~KmzGroundOverlayExport() = default;

KmzGroundOverlayExport::KmzGroundOverlayExport(const QString& path, const Map& map)
: map(map)
, overlap(std::max(2 * (std::nexttoward(180.0, 181.0) - 180.0), 0.000000000000001))
, precision(std::max(std::ceil(log(overlap)/log(0.1) + 0.5), 12.0))
, is_kmz(path.endsWith(QLatin1String(".kmz"), Qt::CaseInsensitive))
{
	auto const fileinfo = QFileInfo(path);
	output_filepath_utf8 = fileinfo.absoluteFilePath().toUtf8();
	if (!is_kmz)
		basepath_utf8 = fileinfo.absolutePath().toUtf8();
}


void KmzGroundOverlayExport::setProgressObserver(QProgressDialog* observer) noexcept
{
	progress_observer = observer;
}

QString KmzGroundOverlayExport::errorString() const
{
	return error_message;
}

bool KmzGroundOverlayExport::wasCanceled() const
{
	return cancelled
	       || (progress_observer && progress_observer->wasCanceled());
}


bool KmzGroundOverlayExport::doExport(MapPrinter& map_printer, int tile_width_px)
{
	error_message.clear();
	cancelled = false;
	staging_filepath_utf8.clear();
	direct_assets_relative_utf8.clear();
	tile_progress_maximum = 1;
	QLockFile output_lock(
		QString::fromUtf8(output_filepath_utf8)
		+ QStringLiteral(".mapper-export.lock"));
	if (!output_lock.tryLock())
	{
		error_message = tr(
			"Another process is already exporting to this destination.");
		return false;
	}
	pruneStagedOutputFiles();

#ifdef QT_PRINTSUPPORT_LIB
	auto const& georef = map.getGeoreferencing();
	if (georef.getState() != Georeferencing::Geospatial)
	{
		error_message = tr("For KML/KMZ export, the map must be georeferenced.");
		return false;
	}
	
	auto const resolution = map_printer.getOptions().resolution * map_printer.getScaleAdjustment();
	if (resolution < 0)
	{
		error_message = tr("Unknown error");
		return false;
	}
	
	// Non-zero declination rotates the original extent box, causing growing bounding boxes.
	auto const bounding_box_lonlat = boundingBoxLonLat(georef, map_printer.getPrintArea());
	auto const bounding_box_map = boundingBoxMap(georef, bounding_box_lonlat);
	auto const metrics = makeMetrics(bounding_box_map.size(), resolution, tile_width_px);
	auto tiles = makeTiles(map_printer.getPrintArea(), metrics);

	auto const exact_output_extent =
		renderExtent(tiles).intersected(
			map_printer.getPrintArea());
	if (!map_printer.prepareOutput(
	    exact_output_extent.isEmpty()
	        ? map_printer.getPrintArea()
	        : exact_output_extent))
	{
		cancelled =
			map_printer.outputWasCanceled() || wasCanceled();
		error_message = cancelled
		              ? tr("Canceled")
		              : map_printer.outputError();
		return false;
	}
	if (wasCanceled())
	{
		cancelled = true;
		error_message = tr("Canceled");
		map_printer.finishOutput(true);
		return false;
	}
	if (!beginStagedOutput(tiles))
	{
		map_printer.finishOutput(true);
		return false;
	}
	
	VSIErrorReset();
	
	// Create the KMZ container file if needed.
	auto* kmz_file = is_kmz ? VSIFOpenL(basepath_utf8, "wb") : nullptr;
	if (is_kmz && !kmz_file)
	{
		error_message = QString::fromUtf8(VSIGetLastErrorMsg());
		map_printer.finishOutput(true);
		cleanupPartialOutput(tiles);
		return false;
	}
	
	// Create KML document and image files.
	setMaximumProgress(int(tiles.size()));
	auto result = doExport(map_printer, metrics, tiles);
	cancelled =
		wasCanceled() || map_printer.outputWasCanceled();
	map_printer.finishOutput(!result || cancelled);
	if (is_kmz)
	{
		if (VSIFCloseL(kmz_file) != 0)
		{
			if (result && !cancelled)
			{
				error_message = tr(
					"Failed to finish the KMZ archive.");
			}
			result = false;
		}
	}

	if (result && !cancelled)
	{
		result = commitStagedOutput();
	}
	if (!result || cancelled)
	{
		cleanupPartialOutput(tiles);
		if (cancelled)
		{
			error_message = tr("Canceled");
		}
	}
	else
	{
		if (progress_observer)
			progress_observer->setValue(100);
	}
	return result && !cancelled;
#else
	Q_UNUSED(map_printer)
	Q_UNUSED(tile_width_px)
	return false;
#endif // QT_PRINTSUPPORT_LIB
}

bool KmzGroundOverlayExport::doExport(MapPrinter& map_printer, const Metrics& metrics, const std::vector<Tile>& tiles)
try
{
#ifdef QT_PRINTSUPPORT_LIB
	if (wasCanceled())
	{
		error_message = tr("Canceled");
		return false;
	}

	// Reusing the same QByteArray allocation for KML document and for tiles.
	QByteArray byte_array;
	byte_array.reserve(100000);
	
	// Create the KML document.
	writeKml(byte_array, tiles);
	writeToVSI(doc_filepath_utf8, byte_array);
	
	// Create the tile files, reusing the same QImage allocation.
	mkdir(basepath_utf8 + "/files");
	
	QImage image(metrics.tile_size_px, QImage::Format_ARGB32_Premultiplied);
	QImage buffer(metrics.tile_size_px, QImage::Format_RGB32);
	if (image.isNull() || buffer.isNull())
	{
		error_message = tr("Failed to allocate a raster export tile.");
		return false;
	}
	auto progress = 0;
	for (auto const& tile : tiles)
	{
		if (wasCanceled())
		{
			error_message = tr("Canceled");
			return false;
		}
		image.fill(Qt::white);
		buffer.fill(Qt::white);
		QPainter painter(&image);
		const auto tile_transform = makeTileTransform(tile.rect_map, metrics, map.getGeoreferencing().getDeclination());
		map_printer.drawPage(
			&painter, renderClip(tile), tile_transform, &buffer);
		if (!painter.isActive())
		{
			error_message = map_printer.outputError().isEmpty()
			              ? tr("Failed to render exact template imagery.")
			              : map_printer.outputError();
			return false;
		}
		painter.end();
		saveToBuffer(image, byte_array);
		writeToVSI(basepath_utf8 + '/' + tile.filepath, byte_array);
		
		setProgress(++progress);
		if (wasCanceled())
		{
			error_message = tr("Canceled");
			return false;
		}
	}
	return true;
#else
	Q_UNUSED(map_printer)
	Q_UNUSED(metrics)
	Q_UNUSED(tiles)
	return false;
#endif  // QT_PRINTSUPPORT_LIB
}
catch (FileFormatException& e)
{
	error_message = e.message();
	return false;
}


std::vector<KmzGroundOverlayExport::Tile> KmzGroundOverlayExport::makeTiles(const QRectF& extent_map, const Metrics& metrics) const
{
	std::vector<Tile> tiles;
	
	auto const& georef = map.getGeoreferencing();
	// Non-zero declination rotates the original extent box, causing growing bounding boxes.
	auto const bounding_box_lonlat = boundingBoxLonLat(georef, extent_map);
	auto const bounding_box_map = boundingBoxMap(georef, bounding_box_lonlat);
	
	auto eastwards = QLineF::fromPolar(metrics.tile_size_mm.width(), georef.getDeclination()).p2();
	auto northwards = QLineF::fromPolar(metrics.tile_size_mm.height(), georef.getDeclination() + 90).p2();
	
	auto const start_map = georef.toMapCoordF(fromLonLat(bounding_box_lonlat.topLeft()));
	auto tile_lonlat = QRectF(bounding_box_lonlat.topLeft(),
	                          toLonLat(georef.toGeographicCoords(MapCoordF(start_map + eastwards + northwards))))
	                   .normalized();
	auto const delta = QPointF(tile_lonlat.width() - overlap, tile_lonlat.height() - overlap);
	
	auto const last_y = int(std::ceil((bounding_box_lonlat.height() - overlap) / delta.y()));
	for (int y = 0; y < last_y; ++y)
	{
		auto const last_x = int(std::ceil((bounding_box_lonlat.width() - overlap) / delta.x()));
		for (int x = 0; x < last_x; ++x)
		{
			MapCoordF tile_map[] = {
			    georef.toMapCoordF(fromLonLat(tile_lonlat.topLeft())),
			    georef.toMapCoordF(fromLonLat(tile_lonlat.topRight())),
			    georef.toMapCoordF(fromLonLat(tile_lonlat.bottomRight())),
			    georef.toMapCoordF(fromLonLat(tile_lonlat.bottomLeft()))
			};
			using std::begin; using std::end;
			if (std::any_of(begin(tile_map), end(tile_map), [&bounding_box_map](auto& p) { return p.x() >= bounding_box_map.left(); })
			    && std::any_of(begin(tile_map), end(tile_map), [&bounding_box_map](auto& p) { return p.x() <= bounding_box_map.right(); })
			    && std::any_of(begin(tile_map), end(tile_map), [&bounding_box_map](auto& p) { return p.y() >= bounding_box_map.top(); })
			    && std::any_of(begin(tile_map), end(tile_map), [&bounding_box_map](auto& p) { return p.y() <= bounding_box_map.bottom(); }))
			{
				auto const name = QByteArray("tile_" + QByteArray::number(x) + '_' + QByteArray::number(y) + ".jpg");
				auto const filepath = QByteArray("files/" + name);
				tiles.push_back({name, filepath, tile_lonlat, boundingBox(tile_map[0], tile_map[1], tile_map[2], tile_map[3])});
			}
			
			tile_lonlat.moveLeft(tile_lonlat.left() + delta.x());
		}
		tile_lonlat.moveLeft(bounding_box_lonlat.left());
		tile_lonlat.moveTop(tile_lonlat.top() + delta.y());
	}
	return tiles;
}


QRectF KmzGroundOverlayExport::renderClip(const Tile& tile) noexcept
{
	// Include nearby objects and template pixels which can contribute through
	// antialiasing at the edge of the geographic tile.
	return tile.rect_map.adjusted(-5, -5, 5, 5);
}

QRectF KmzGroundOverlayExport::renderExtent(
	const std::vector<Tile>& tiles) noexcept
{
	if (tiles.empty())
		return {};
	auto extent = renderClip(tiles.front());
	for (auto tile = std::next(tiles.cbegin()); tile != tiles.cend(); ++tile)
		extent = extent.united(renderClip(*tile));
	return extent;
}


void KmzGroundOverlayExport::writeKml(QByteArray& buffer, const std::vector<KmzGroundOverlayExport::Tile>& tiles) const
{
	buffer.append(
	  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	  "<!-- Generator: OpenOrienteering Mapper " APP_VERSION " -->\n"
	  "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n"
	  "<Folder>\n"
	  " <name>Map</name>\n"
	);
	for (auto const& tile : tiles)
	{
		buffer.append(
		  " <GroundOverlay id=\"").append(tile.name).append("\">\n"
		  "  <name>").append(tile.name).append("</name>\n"
		  "  <Icon>\n"
		  "   <href>").append(tile.filepath).append("</href>\n"
		  "  </Icon>\n"
		  "  <LatLonBox>\n"
		  "   <north>").append(QByteArray::number(tile.rect_lonlat.bottom(), 'f', precision)).append("</north>\n"
		  "   <south>").append(QByteArray::number(tile.rect_lonlat.top(), 'f', precision)).append("</south>\n"
		  "   <east>").append(QByteArray::number(tile.rect_lonlat.right(), 'f', precision)).append("</east>\n"
		  "   <west>").append(QByteArray::number(tile.rect_lonlat.left(), 'f', precision)).append("</west>\n"
		  "   <rotation>0</rotation>\n"
		  "  </LatLonBox>\n"
		  " </GroundOverlay>\n"
		);
	}
	buffer.append(
	  "</Folder>\n"
	  "</kml>\n"
	);
}


// static
QTransform KmzGroundOverlayExport::makeTileTransform(const QRectF& tile_map, const KmzGroundOverlayExport::Metrics& metrics, qreal const declination)
{
	auto transform = QTransform::fromScale(metrics.units_per_mm, metrics.units_per_mm);
	transform.translate(metrics.tile_size_mm.width() / 2, metrics.tile_size_mm.height() / 2);
	transform.rotate(declination);
	transform.translate(-tile_map.left() - tile_map.width() / 2, -tile_map.top() - tile_map.height() / 2);
	return transform;
}

// static
void KmzGroundOverlayExport::saveToBuffer(const QImage& image, QByteArray& data)
{
	QBuffer buffer(&data);
	if (!buffer.open(QIODevice::WriteOnly | QIODevice::Truncate)
	    || !image.save(&buffer, format, quality))
	{
		throw FileFormatException(
			KmzGroundOverlayExport::tr(
				"Failed to encode a raster export tile."));
	}
}

// static
void KmzGroundOverlayExport::writeToVSI(const QByteArray& filepath_utf8, const QByteArray& data)
{
	auto* file = VSIFOpenL(filepath_utf8, "wb");
	if (!file)
		throw FileFormatException(QString::fromUtf8(VSIGetLastErrorMsg()));
	qsizetype offset = 0;
	bool write_failed = false;
	while (offset < data.size())
	{
		auto const written = VSIFWriteL(
			data.constData() + offset,
			1,
			std::size_t(data.size() - offset),
			file);
		if (written == 0)
		{
			write_failed = true;
			break;
		}
		offset += qsizetype(written);
	}
	auto const close_failed = VSIFCloseL(file) != 0;
	if (write_failed || close_failed)
	{
		auto error = QString::fromUtf8(VSIGetLastErrorMsg());
		if (error.isEmpty())
			error = KmzGroundOverlayExport::tr(
				"Failed to write an export file.");
		throw FileFormatException(error);
	}
}

void KmzGroundOverlayExport::mkdir(const QByteArray& path_utf8) const
{
	// We must not read/stat kmz during creation.
	if ((is_kmz || !GdalFile::isDir(path_utf8))
	    && !GdalFile::mkdir(path_utf8))
	{
		throw FileFormatException(QString::fromUtf8(VSIGetLastErrorMsg()));
	}
}


void KmzGroundOverlayExport::setMaximumProgress(int value)
{
	tile_progress_maximum = std::max(1, value);
	if (progress_observer)
		progress_observer->setRange(0, 100);
}

void KmzGroundOverlayExport::setProgress(int value) const
{
	if (progress_observer)
	{
		auto const percent =
			15
			+ (84 * std::clamp(
			     value, 0, tile_progress_maximum))
			  / tile_progress_maximum;
		progress_observer->setValue(percent);
		QApplication::processEvents(
			QEventLoop::AllEvents, 100 /* ms */);
	}
}

bool KmzGroundOverlayExport::beginStagedOutput(
	std::vector<Tile>& tiles)
{
	auto const output_path =
		QString::fromUtf8(output_filepath_utf8);
	auto const fileinfo = QFileInfo(output_path);
	auto const staging_prefix = QString::fromLatin1(
		stagingFilePrefix(output_filepath_utf8, is_kmz));
	QTemporaryFile staging(
		fileinfo.absolutePath()
		+ QLatin1Char('/')
		+ staging_prefix
		+ QStringLiteral("XXXXXX")
		// /vsizip/ treats the first .zip/.kmz component as the archive
		// boundary, so only the final suffix may contain .kmz.
		+ (is_kmz
		       ? QStringLiteral(".kmz")
		       : QStringLiteral(".part")));
	staging.setAutoRemove(true);
	if (!staging.open())
	{
		error_message = tr("Could not create a temporary export file: %1")
			.arg(staging.errorString());
		return false;
	}
	staging_filepath_utf8 =
		staging.fileName().toUtf8();
	staging.close();

	if (is_kmz)
	{
		if (!QFile::remove(
			    QString::fromUtf8(staging_filepath_utf8)))
		{
			error_message = tr(
				"Could not initialize the temporary KMZ archive.");
			return false;
		}
		basepath_utf8 =
			"/vsizip/" + staging_filepath_utf8;
		doc_filepath_utf8 =
			basepath_utf8 + "/doc.kml";
		files_directory_existed = false;
	}
	else
	{
		basepath_utf8 = fileinfo.absolutePath().toUtf8();
		doc_filepath_utf8 = staging_filepath_utf8;
		files_directory_existed =
			GdalFile::isDir(basepath_utf8 + "/files");
		auto const files_path =
			fileinfo.absolutePath()
			+ QStringLiteral("/files");
		if (!QDir().mkpath(files_path))
		{
			error_message = tr(
				"Could not create the KML image directory.");
			staging_filepath_utf8.clear();
			return false;
		}

		// Direct KML is a multi-file format. Keep every new sidecar in an
		// exporter-owned transaction directory and publish the KML document
		// last. On the next run, the currently published KML identifies the
		// one live transaction directory; any other such directories are
		// crash leftovers and can be removed safely.
		pruneDirectAssetDirectories();
		auto const asset_prefix =
			directAssetPrefix(output_filepath_utf8);
		QTemporaryDir assets(
			files_path
			+ QLatin1Char('/')
			+ QString::fromLatin1(asset_prefix)
			+ QStringLiteral("XXXXXX"));
		assets.setAutoRemove(true);
		if (!assets.isValid())
		{
			error_message = tr(
				"Could not create a temporary KML image directory.");
			staging_filepath_utf8.clear();
			return false;
		}
		auto const assets_name =
			QFileInfo(assets.path()).fileName();
		direct_assets_relative_utf8 =
			QByteArray("files/")
			+ assets_name.toUtf8();
		for (auto& tile : tiles)
			tile.filepath =
				direct_assets_relative_utf8
				+ '/' + tile.name;
		assets.setAutoRemove(false);
		staging.setAutoRemove(false);
	}
	return true;
}

bool KmzGroundOverlayExport::commitStagedOutput()
{
	QFile source(QString::fromUtf8(staging_filepath_utf8));
	if (!source.open(QIODevice::ReadOnly))
	{
		error_message = tr("Could not read the completed temporary export: %1")
			.arg(source.errorString());
		return false;
	}
	QSaveFile destination(
		QString::fromUtf8(output_filepath_utf8));
	if (!destination.open(QIODevice::WriteOnly))
	{
		error_message = tr("Could not open the export destination: %1")
			.arg(destination.errorString());
		return false;
	}

	QByteArray buffer(256 * 1024, Qt::Uninitialized);
	while (true)
	{
		auto const count =
			source.read(buffer.data(), buffer.size());
		if (count == 0)
			break;
		if (count < 0)
		{
			error_message = tr("Could not read the completed temporary export: %1")
				.arg(source.errorString());
			destination.cancelWriting();
			return false;
		}
		if (destination.write(buffer.constData(), count)
		    != count)
		{
			error_message = tr("Could not write the export destination: %1")
				.arg(destination.errorString());
			destination.cancelWriting();
			return false;
		}
		QApplication::processEvents(
			QEventLoop::AllEvents, 25);
		if (wasCanceled())
		{
			cancelled = true;
			error_message = tr("Canceled");
			destination.cancelWriting();
			return false;
		}
	}
	if (!destination.commit())
	{
		error_message = tr("Could not finish the export destination: %1")
			.arg(destination.errorString());
		return false;
	}
	source.close();
	QFile::remove(
		QString::fromUtf8(staging_filepath_utf8));
	staging_filepath_utf8.clear();
	if (!is_kmz)
	{
		pruneDirectAssetDirectories(
			direct_assets_relative_utf8);
		direct_assets_relative_utf8.clear();
	}
	return true;
}

void KmzGroundOverlayExport::cleanupPartialOutput(
	const std::vector<Tile>& tiles)
{
	if (!staging_filepath_utf8.isEmpty())
	{
		QFile::remove(
			QString::fromUtf8(staging_filepath_utf8));
		staging_filepath_utf8.clear();
	}
	if (is_kmz)
		return;
	Q_UNUSED(tiles)
	if (!direct_assets_relative_utf8.isEmpty())
	{
		QDir(
			QString::fromUtf8(
				basepath_utf8 + '/'
				+ direct_assets_relative_utf8))
			.removeRecursively();
		direct_assets_relative_utf8.clear();
	}
	if (!files_directory_existed)
	{
		auto const files_path = QByteArray(basepath_utf8 + "/files");
		VSIRmdir(files_path.constData());
	}
}

void KmzGroundOverlayExport::pruneStagedOutputFiles()
{
	auto const output_path =
		QString::fromUtf8(output_filepath_utf8);
	auto const fileinfo = QFileInfo(output_path);
	QDir directory(fileinfo.absolutePath());
	if (!directory.exists())
		return;

	auto const prefix = QString::fromLatin1(
		stagingFilePrefix(output_filepath_utf8, is_kmz));
	auto const suffix = is_kmz
	                  ? QStringLiteral(".kmz")
	                  : QStringLiteral(".part");
	auto const entries = directory.entryInfoList(
		{ prefix + QLatin1Char('*') + suffix },
		QDir::Files | QDir::Hidden | QDir::NoSymLinks);
	for (auto const& entry : entries)
	{
		// Never follow or unlink a caller-provided symlink or special file.
		if (entry.isFile() && !entry.isSymLink())
			QFile::remove(entry.absoluteFilePath());
	}
}

void KmzGroundOverlayExport::pruneDirectAssetDirectories(
	const QByteArray& keep_relative_path)
{
	if (is_kmz)
		return;
	auto const files_path =
		QString::fromUtf8(basepath_utf8 + "/files");
	QDir files(files_path);
	if (!files.exists())
		return;

	QByteArray published_kml;
	auto can_identify_published_assets =
		!keep_relative_path.isEmpty();
	if (!can_identify_published_assets)
	{
		QFile current(
			QString::fromUtf8(output_filepath_utf8));
		constexpr qint64 maximum_kml_recovery_bytes =
			qint64(16) * 1024 * 1024;
		if (!current.exists())
		{
			can_identify_published_assets = true;
		}
		else if (current.size()
		             <= maximum_kml_recovery_bytes
		         && current.open(QIODevice::ReadOnly))
		{
			published_kml =
				current.read(
					maximum_kml_recovery_bytes + 1);
			can_identify_published_assets =
				published_kml.size()
				<= maximum_kml_recovery_bytes;
		}
	}
	if (!can_identify_published_assets)
		return;

	auto const entries = files.entryInfoList(
		{ QString::fromLatin1(
			directAssetPrefix(output_filepath_utf8))
		  + QLatin1Char('*') },
		QDir::Dirs | QDir::Hidden
			| QDir::NoDotAndDotDot
			| QDir::NoSymLinks);
	for (auto const& entry : entries)
	{
		QByteArray const relative =
			QByteArray("files/")
			+ entry.fileName().toUtf8();
		auto const keep =
			!keep_relative_path.isEmpty()
				? relative == keep_relative_path
				: published_kml.contains(relative);
		if (!keep)
			QDir(entry.absoluteFilePath())
				.removeRecursively();
	}
}


}  // namespace OpenOrienteering
