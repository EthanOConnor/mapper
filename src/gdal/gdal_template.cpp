/*
 *    Copyright 2019, 2020 Kai Pastor
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

#include "gdal_template.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>

#include <Qt>
#include <QtGlobal>
#include <QByteArray>
#include <QChar>
#include <QElapsedTimer>
#include <QImageReader>
#include <QMetaObject>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QVariant>

#include <cpl_conv.h>
#include <gdal.h>

#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map.h"
#include "core/map_coord.h"
#include "gdal/gdal_file.h"
#include "gdal/gdal_image_reader.h"
#include "gdal/gdal_manager.h"
#include "util/transformation.h"
#include "util/util.h"


namespace OpenOrienteering {

// static
bool GdalTemplate::canRead(const QString& path)
{
	return GdalImageReader(path).canRead();
}


// static
const std::vector<QByteArray>& GdalTemplate::supportedExtensions()
{
	return GdalManager().supportedRasterExtensions();
}


// static
const char* GdalTemplate::applyCornerPassPointsProperty()
{
	return "GdalTemplate::applyCornerPassPoints";
}


GdalTemplate::GdalTemplate(const QString& path, Map* map)
: TemplateImage(path, map)
{}

GdalTemplate::GdalTemplate(const GdalTemplate& proto)
: TemplateImage(proto)
// Tiled state is NOT copied. The duplicate will re-load from the file.
// std::thread, std::mutex, etc. are not copyable, and each instance
// needs its own dataset handle and worker lifecycle.
{}

GdalTemplate::~GdalTemplate()
{
	// Ensure the worker thread is stopped before members are destroyed.
	if (worker_thread.joinable())
	{
		worker_stop = true;
		queue_cv.notify_one();
		worker_thread.join();
	}
	if (tiled_dataset)
	{
		GDALClose(tiled_dataset);
		tiled_dataset = nullptr;
	}
}

GdalTemplate* GdalTemplate::duplicate() const
{
	return new GdalTemplate(*this);
}

const char* GdalTemplate::getTemplateType() const
{
	return "GdalTemplate";
}


Template::LookupResult GdalTemplate::tryToFindTemplateFile(const QString& map_path)
{
	auto template_path_utf8 = template_path.toUtf8();
	if (GdalFile::isRelative(template_path_utf8))
	{
		auto absolute_path_utf8 = GdalFile::tryToFindRelativeTemplateFile(template_path_utf8, map_path.toUtf8());
		if (!absolute_path_utf8.isEmpty())
		{
			setTemplatePath(QString::fromUtf8(absolute_path_utf8));
			return FoundByRelPath;
		}
	}

	if (GdalFile::exists(template_path_utf8))
	{
		return FoundByAbsPath;
	}

	return TemplateImage::tryToFindTemplateFile(map_path);
}

bool GdalTemplate::fileExists() const
{
	return GdalFile::exists(getTemplatePath().toUtf8())
	       || TemplateImage::fileExists();
}


bool GdalTemplate::isTiledSource() const
{
	return tiled_dataset != nullptr;
}


bool GdalTemplate::loadTemplateFileImpl()
{
	GdalImageReader reader(template_path);
	if (!reader.canRead())
	{
		setErrorString(reader.errorString());
		return false;
	}

	qDebug("GdalTemplate: Using GDAL driver '%s'", reader.format().constData());

	auto raster_info = reader.readRasterInfo();

	// Tiled path: source has a meaningful block size (WMS, MBTiles, COG, etc.)
	if (!raster_info.block_size.isEmpty()
	    && raster_info.image_format != QImage::Format_Invalid)
	{
		qDebug("GdalTemplate: Tiled source detected, block size %dx%d, raster %dx%d",
		       raster_info.block_size.width(), raster_info.block_size.height(),
		       raster_info.size.width(), raster_info.size.height());

		// Keep our own dataset handle for the tile worker thread.
		GdalManager();
		CPLErrorReset();
		tiled_dataset = GDALOpen(template_path.toUtf8(), GA_ReadOnly);
		if (!tiled_dataset)
		{
			setErrorString(tr("Failed to open tiled raster: %1")
			               .arg(QString::fromUtf8(CPLGetLastErrorMsg())));
			return false;
		}

		tiled_raster_size = raster_info.size;

		// For indexed-color (palette) images, pre-fetch the color table
		// so the postprocessing lambda doesn't capture the GdalImageReader
		// which will be destroyed when this function returns.
		if (raster_info.image_format == QImage::Format_Indexed8
		    && !raster_info.bands.empty())
		{
			auto color_table = reader.readColorTable(raster_info.bands.front());
			raster_info.postprocessing = [color_table](QImage& img) {
				img.setColorTable(color_table);
			};
		}

		tiled_raster_info = raster_info;

		// Set up georeferencing from the GDAL geotransform.
		// We use setupTiledGeoreferencing() instead of calculateGeoreferencing()
		// to avoid allocating a full-size QImage for the raster dimensions.
		auto georef_option = reader.readGeoTransform();
		available_georef = findAvailableGeoreferencing(georef_option);
		if (is_georeferenced)
		{
			if (!isGeoreferencingUsable())
			{
				setErrorString(::OpenOrienteering::TemplateImage::tr("Georeferencing not found"));
				GDALClose(tiled_dataset);
				tiled_dataset = nullptr;
				return false;
			}

			setupTiledGeoreferencing();
		}
		else if (property(applyCornerPassPointsProperty()).toBool())
		{
			if (!applyCornerPassPoints())
			{
				GDALClose(tiled_dataset);
				tiled_dataset = nullptr;
				return false;
			}
		}

		// Start the background tile worker.
		worker_stop = false;
		worker_thread = std::thread(&GdalTemplate::tileWorkerLoop, this);

		return true;
	}

	// Non-tiled path: load the full image (existing behavior, unchanged).
	if (!reader.read(&image))
	{
		setErrorString(reader.errorString());

		QImageReader image_reader(template_path);
		if (image_reader.canRead())
		{
			qDebug("GdalTemplate: Falling back to QImageReader, reason: %s", qPrintable(errorString()));
			if (!image_reader.read(&image))
			{
				setErrorString(errorString() + QChar::LineFeed + image_reader.errorString());
				return false;
			}
		}
	}

	// Duplicated from TemplateImage, for compatibility
	available_georef = findAvailableGeoreferencing(reader.readGeoTransform());
	if (is_georeferenced)
	{
		if (!isGeoreferencingUsable())
		{
			// Image was georeferenced, but georeferencing info is gone -> deny to load template
			setErrorString(::OpenOrienteering::TemplateImage::tr("Georeferencing not found"));
			return false;
		}

		calculateGeoreferencing();
	}
	else if (property(applyCornerPassPointsProperty()).toBool())
	{
		if (!applyCornerPassPoints())
			return false;
	}

	return true;
}


void GdalTemplate::unloadTemplateFileImpl()
{
	// Stop the tile worker thread.
	if (worker_thread.joinable())
	{
		worker_stop = true;
		queue_cv.notify_one();
		worker_thread.join();
	}

	// Release GDAL dataset.
	if (tiled_dataset)
	{
		GDALClose(tiled_dataset);
		tiled_dataset = nullptr;
	}

	// Clear tile cache and state.
	tile_cache.clear();
	loading_tiles.clear();
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		tile_queue.clear();
	}

	tiled_raster_info = {};
	tiled_raster_size = {};

	TemplateImage::unloadTemplateFileImpl();
}


void GdalTemplate::drawTemplate(QPainter* painter, const QRectF& clip_rect, double scale, bool on_screen, qreal opacity) const
{
	// Non-tiled path: delegate to TemplateImage (existing behavior, untouched).
	if (!tiled_dataset)
	{
		TemplateImage::drawTemplate(painter, clip_rect, scale, on_screen, opacity);
		return;
	}

#ifdef Mapper_DEVELOPMENT_BUILD
	QElapsedTimer perf_timer;
	perf_timer.start();
	int tiles_drawn = 0;
	int tiles_requested = 0;
#endif

	applyTemplateTransform(painter);

	painter->setRenderHint(QPainter::SmoothPixmapTransform);
	painter->setOpacity(opacity);

	// Determine the visible area in template pixel coordinates.
	// The painter transform maps template pixels → device pixels.
	// We need the inverse: device/map clip_rect → template pixels.
	auto const inv_transform = painter->transform().inverted();
	auto const visible_rect = inv_transform.mapRect(clip_rect);

	// Template pixel origin is at (-width/2, -height/2) because
	// TemplateImage centers the image. Convert to raster pixel coords.
	auto const half_w = tiled_raster_size.width() * 0.5;
	auto const half_h = tiled_raster_size.height() * 0.5;

	auto const block_w = tiled_raster_info.block_size.width();
	auto const block_h = tiled_raster_info.block_size.height();

	// Determine the tile grid range that covers the visible area, plus 1 tile overscan.
	int tile_x_min = int(std::floor((visible_rect.left() + half_w) / block_w)) - 1;
	int tile_y_min = int(std::floor((visible_rect.top() + half_h) / block_h)) - 1;
	int tile_x_max = int(std::floor((visible_rect.right() + half_w) / block_w)) + 1;
	int tile_y_max = int(std::floor((visible_rect.bottom() + half_h) / block_h)) + 1;

	// Clamp to raster bounds.
	int const max_tile_x = (tiled_raster_size.width() - 1) / block_w;
	int const max_tile_y = (tiled_raster_size.height() - 1) / block_h;
	tile_x_min = std::max(tile_x_min, 0);
	tile_y_min = std::max(tile_y_min, 0);
	tile_x_max = std::min(tile_x_max, max_tile_x);
	tile_y_max = std::min(tile_y_max, max_tile_y);

	// Draw cached tiles and request missing ones.
	for (int ty = tile_y_min; ty <= tile_y_max; ++ty)
	{
		for (int tx = tile_x_min; tx <= tile_x_max; ++tx)
		{
			auto const key = tileKey(tx, ty);
			auto it = tile_cache.constFind(key);
			if (it != tile_cache.constEnd())
			{
				// Draw cached tile at its position in template pixel space.
				auto const px = tx * block_w - half_w;
				auto const py = ty * block_h - half_h;
				painter->drawImage(QPointF(px, py), it.value());
#ifdef Mapper_DEVELOPMENT_BUILD
				++tiles_drawn;
#endif
			}
			else
			{
				requestTile(tx, ty);
#ifdef Mapper_DEVELOPMENT_BUILD
				++tiles_requested;
#endif
			}
		}
	}

	painter->setRenderHint(QPainter::SmoothPixmapTransform, false);

#ifdef Mapper_DEVELOPMENT_BUILD
	qDebug("GdalTemplate::drawTemplate: %lld ms, %d tiles drawn, %d requested",
	       perf_timer.elapsed(), tiles_drawn, tiles_requested);
#endif
}


QRectF GdalTemplate::getTemplateExtent() const
{
	if (!tiled_dataset)
		return TemplateImage::getTemplateExtent();

	// Template pixel space is centered: origin at the middle of the raster.
	auto const w = tiled_raster_size.width();
	auto const h = tiled_raster_size.height();
	return QRectF(-w * 0.5, -h * 0.5, w, h);
}


// --- Background tile loading ---

void GdalTemplate::tileWorkerLoop()
{
	while (true)
	{
		QPoint tile_coord;
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			queue_cv.wait(lock, [this] {
				return !tile_queue.isEmpty() || worker_stop.load();
			});
			if (worker_stop.load())
				return;
			tile_coord = tile_queue.dequeue();
		}

		auto const block_w = tiled_raster_info.block_size.width();
		auto const block_h = tiled_raster_info.block_size.height();

		// Determine the pixel rectangle for this tile, clamped to raster bounds.
		int const px = tile_coord.x() * block_w;
		int const py = tile_coord.y() * block_h;
		int const read_w = std::min(block_w, tiled_raster_size.width() - px);
		int const read_h = std::min(block_h, tiled_raster_size.height() - py);

		if (read_w <= 0 || read_h <= 0)
			continue;

		QImage tile(read_w, read_h, tiled_raster_info.image_format);
		if (tile.isNull())
			continue;

		tile.fill(Qt::white);

		CPLErrorReset();
		auto result = GDALDatasetRasterIO(
			tiled_dataset, GF_Read,
			px, py, read_w, read_h,
			tile.bits() + tiled_raster_info.band_offset, read_w, read_h,
			GDT_Byte,
			tiled_raster_info.bands.count(), tiled_raster_info.bands.data(),
			tiled_raster_info.pixel_space, tile.bytesPerLine(),
			tiled_raster_info.band_space);

		if (result >= CE_Warning)
		{
			qDebug("GdalTemplate: Tile read failed at (%d,%d): %s",
			       tile_coord.x(), tile_coord.y(),
			       CPLGetLastErrorMsg());
			continue;
		}

		tiled_raster_info.postprocessing(tile);

		auto const key = tileKey(tile_coord.x(), tile_coord.y());

		// Post the loaded tile to the UI thread.
		QMetaObject::invokeMethod(
			const_cast<GdalTemplate*>(this),
			[this, key, tile = std::move(tile)]() mutable {
				onTileLoaded(key, std::move(tile));
			},
			Qt::QueuedConnection);
	}
}


void GdalTemplate::requestTile(int tile_x, int tile_y) const
{
	auto const key = tileKey(tile_x, tile_y);
	if (loading_tiles.contains(key))
		return;

	loading_tiles.insert(key);
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		tile_queue.enqueue(QPoint(tile_x, tile_y));
	}
	queue_cv.notify_one();
}


void GdalTemplate::onTileLoaded(quint64 key, QImage tile_image)
{
	// Simple LRU-like eviction: if over budget, drop random entries.
	// A proper LRU would track access order, but for a first implementation,
	// clearing the oldest half is good enough.
	if (tile_cache.size() >= tile_cache_budget)
	{
		auto it = tile_cache.begin();
		int to_remove = tile_cache.size() / 4;
		while (to_remove > 0 && it != tile_cache.end())
		{
			auto const evict_key = it.key();
			it = tile_cache.erase(it);
			loading_tiles.remove(evict_key);
			--to_remove;
		}
	}

	tile_cache.insert(key, std::move(tile_image));
	loading_tiles.remove(key);

	// Trigger repaint of the template's visible area.
	setTemplateAreaDirty();
}


// static
quint64 GdalTemplate::tileKey(int tile_x, int tile_y)
{
	return (quint64(quint32(tile_x)) << 32) | quint64(quint32(tile_y));
}


void GdalTemplate::setupTiledGeoreferencing()
{
	if (!isGeoreferencingUsable())
	{
		qWarning("%s must not be called with incomplete georeferencing", Q_FUNC_INFO);
		return;
	}

	// Same as TemplateImage::calculateGeoreferencing(), but the pass-point
	// math uses tiled_raster_size instead of image.width()/height().
	// This avoids allocating a full-size QImage for huge raster sources.
	georef = std::make_unique<Georeferencing>();
	georef->setProjectedCRS(QString{}, available_georef.effective.crs_spec);
	georef->setTransformationDirectly(available_georef.effective.transform.pixel_to_world);

	if (map->getGeoreferencing().getState() != Georeferencing::Geospatial)
		return;

	auto const w = double(tiled_raster_size.width());
	auto const h = double(tiled_raster_size.height());

	bool ok;
	MapCoordF top_left = map->getGeoreferencing().toMapCoordF(georef.get(), MapCoordF(0.0, 0.0), &ok);
	if (!ok) { qDebug("%s: top_left failed", Q_FUNC_INFO); return; }
	MapCoordF top_right = map->getGeoreferencing().toMapCoordF(georef.get(), MapCoordF(w, 0.0), &ok);
	if (!ok) { qDebug("%s: top_right failed", Q_FUNC_INFO); return; }
	MapCoordF bottom_left = map->getGeoreferencing().toMapCoordF(georef.get(), MapCoordF(0.0, h), &ok);
	if (!ok) { qDebug("%s: bottom_left failed", Q_FUNC_INFO); return; }

	PassPointList pp_list;
	PassPoint pp;
	pp.src_coords = MapCoordF(-0.5 * w, -0.5 * h);
	pp.dest_coords = top_left;
	pp_list.push_back(pp);
	pp.src_coords = MapCoordF(0.5 * w, -0.5 * h);
	pp.dest_coords = top_right;
	pp_list.push_back(pp);
	pp.src_coords = MapCoordF(-0.5 * w, 0.5 * h);
	pp.dest_coords = bottom_left;
	pp_list.push_back(pp);

	QTransform q_transform;
	if (!pp_list.estimateNonIsometricSimilarityTransform(&q_transform))
	{
		qDebug("%s: transform estimation failed", Q_FUNC_INFO);
		return;
	}
	transform = TemplateTransform::fromQTransform(q_transform);
	updateTransformationMatrices();
}


bool GdalTemplate::applyCornerPassPoints()
{
	if (passpoints.empty())
		return false;

	using std::begin; using std::end;

	// Find the center of the destination coords, to be used as pivotal point.
	auto const first = map->getGeoreferencing().toGeographicCoords(passpoints.front().dest_coords);
	auto lonlat_box = QRectF{first.longitude(), first.latitude(), 0, 0};
	std::for_each(begin(passpoints)+1, end(passpoints), [this, &lonlat_box](auto const& pp) {
		auto const latlon = map->getGeoreferencing().toGeographicCoords(pp.dest_coords);
		rectInclude(lonlat_box, QPointF{latlon.longitude(), latlon.latitude()});
	});
	auto const center = [](auto c) { return LatLon(c.y(), c.x()); } (lonlat_box.center());

	// Determine src_coords for each dest_coords, assuming corners relative to the center.
	auto const current = calculateTemplateBoundingBox();
	for (auto& pp : passpoints)
	{
		auto dest_latlon = map->getGeoreferencing().toGeographicCoords(pp.dest_coords);
		if (dest_latlon.longitude() < center.longitude())
		{
			if (dest_latlon.latitude() > center.latitude())
				pp.src_coords = current.topLeft();
			else
				pp.src_coords = current.bottomLeft();
		}
		else
		{
			if (dest_latlon.latitude() > center.latitude())
				pp.src_coords = current.topRight();
			else
				pp.src_coords = current.bottomRight();
		}
	}

	TemplateTransform corner_alignment;
	if (!passpoints.estimateSimilarityTransformation(&corner_alignment))
	{
		qDebug("%s: Failed to calculate the KML overlay raster positioning", qUtf8Printable(getTemplatePath()));
		return false;
	}

	// Apply transform directly, without further signals at this stage.
	setProperty("GdalTemplate::applyPassPoints", false);
	passpoints.clear();
	transform = corner_alignment;
	updateTransformationMatrices();
	return true;
}


}  // namespace OpenOrienteering
