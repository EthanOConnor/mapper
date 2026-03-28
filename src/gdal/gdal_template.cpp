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
#include <tuple>
#include <limits>

#include <Qt>
#include <QtGlobal>
#include <QByteArray>
#include <QChar>
#include <QDebug>
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
#include "gui/util_gui.h"
#include "util/transformation.h"
#include "util/util.h"


namespace OpenOrienteering {

namespace {

bool shouldDebugGdalTemplate()
{
	static bool const enabled = qEnvironmentVariableIsSet("MAPPER_GDAL_DEBUG");
	return enabled;
}

int tileSubsamplingForScale(double scale, const QSize& block_size)
{
	if (!(scale > 0.0) || block_size.isEmpty())
		return 1;

	auto const max_subsampling = std::max(1, std::min(block_size.width(), block_size.height()));
	int subsampling = 1;
	while (subsampling * 2 <= max_subsampling
	       && scale * (subsampling * 2) <= 1.0)
	{
		subsampling *= 2;
	}
	return subsampling;
}

qsizetype tileByteCost(const QImage& tile)
{
	return qsizetype(tile.bytesPerLine()) * tile.height();
}

struct RasterIoCancellationContext
{
	std::atomic<bool>* stop_flag = nullptr;
	std::atomic<quint64>* active_request_generation = nullptr;
	quint64 request_generation = 0;
};

int tileReadProgress(double /*complete*/, const char* /*message*/, void* user_data)
{
	auto* context = static_cast<RasterIoCancellationContext*>(user_data);
	if (!context)
		return true;

	if (context->stop_flag && context->stop_flag->load(std::memory_order_relaxed))
		return false;

	if (context->active_request_generation
	    && context->active_request_generation->load(std::memory_order_relaxed) != context->request_generation)
	{
		return false;
	}

	return true;
}

}  // namespace

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
	if (!worker_threads.empty())
	{
		worker_stop = true;
		queue_cv.notify_all();
		for (auto& worker_thread : worker_threads)
		{
			if (worker_thread.joinable())
				worker_thread.join();
		}
		worker_threads.clear();
	}
	for (auto worker_dataset : worker_datasets)
	{
		if (worker_dataset)
			GDALClose(worker_dataset);
	}
	worker_datasets.clear();
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
		if (shouldDebugGdalTemplate())
		{
			debug_draw_logs_remaining = 8;
			debug_tile_logs_remaining = 8;
		}

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

		// Start the background tile workers.
		worker_stop = false;
		worker_datasets.clear();
		worker_datasets.reserve(worker_thread_count);
		for (int i = 0; i < worker_thread_count; ++i)
		{
			CPLErrorReset();
			auto worker_dataset = GDALOpen(template_path.toUtf8(), GA_ReadOnly);
			if (!worker_dataset)
			{
				setErrorString(tr("Failed to open tiled raster worker: %1")
				               .arg(QString::fromUtf8(CPLGetLastErrorMsg())));
				for (auto opened_dataset : worker_datasets)
				{
					if (opened_dataset)
						GDALClose(opened_dataset);
				}
				worker_datasets.clear();
				GDALClose(tiled_dataset);
				tiled_dataset = nullptr;
				return false;
			}
			worker_datasets.push_back(worker_dataset);
		}

		worker_threads.clear();
		worker_threads.reserve(worker_thread_count);
		for (int i = 0; i < worker_thread_count; ++i)
			worker_threads.emplace_back(&GdalTemplate::tileWorkerLoop, this, worker_datasets[i]);

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
	// Stop the tile worker threads.
	if (!worker_threads.empty())
	{
		worker_stop = true;
		queue_cv.notify_all();
		for (auto& worker_thread : worker_threads)
		{
			if (worker_thread.joinable())
				worker_thread.join();
		}
		worker_threads.clear();
	}
	for (auto worker_dataset : worker_datasets)
	{
		if (worker_dataset)
			GDALClose(worker_dataset);
	}
	worker_datasets.clear();

	// Release GDAL dataset.
	if (tiled_dataset)
	{
		GDALClose(tiled_dataset);
		tiled_dataset = nullptr;
	}

	// Clear tile cache and state.
	tile_cache.clear();
	tile_cache_access.clear();
	tile_cache_bytes = 0;
	loading_tiles.clear();
	active_subsampling.store(0, std::memory_order_relaxed);
	active_request_generation.store(0, std::memory_order_relaxed);
	current_request_window = {};
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
	// clip_rect is passed in map coordinates, not device/view coordinates,
	// so we must transform it with the template's map<->template matrices.
	QRectF visible_rect;
	rectIncludeSafe(visible_rect, mapToTemplate(MapCoordF(clip_rect.topLeft())));
	rectIncludeSafe(visible_rect, mapToTemplate(MapCoordF(clip_rect.topRight())));
	rectIncludeSafe(visible_rect, mapToTemplate(MapCoordF(clip_rect.bottomLeft())));
	rectIncludeSafe(visible_rect, mapToTemplate(MapCoordF(clip_rect.bottomRight())));

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
	auto effective_scale = scale;
	if (on_screen)
	{
		effective_scale = Util::mmToPixelPhysical(scale);
	}
	else
	{
		auto dpi = painter->device()->physicalDpiX();
		if (!dpi)
			dpi = painter->device()->logicalDpiX();
		if (dpi > 0)
			effective_scale *= dpi / 25.4;
	}

	auto const subsampling = chooseTileSubsampling(effective_scale, tiled_raster_info.block_size);

	if (shouldDebugGdalTemplate() && debug_draw_logs_remaining > 0)
	{
		--debug_draw_logs_remaining;
		auto const bbox = calculateTemplateBoundingBox();
		qInfo().noquote()
			<< "GdalTemplate draw"
			<< "path=" << template_path
			<< "clip_rect=" << clip_rect
			<< "visible_rect=" << visible_rect
			<< "bbox=" << bbox
			<< "scale=(" << transform.template_scale_x << "," << transform.template_scale_y << ")"
			<< "translation=(" << transform.template_x / 1000.0 << "," << transform.template_y / 1000.0 << ")"
			<< "subsampling=" << subsampling
			<< "tiles=" << QStringLiteral("[%1..%2]x[%3..%4]")
			              .arg(tile_x_min).arg(tile_x_max).arg(tile_y_min).arg(tile_y_max);
	}

	auto const request_generation = beginRequestGeneration(RequestWindow{
		tile_x_min, tile_y_min, tile_x_max, tile_y_max, subsampling
	});

	// Draw cached tiles and request missing ones.
	struct MissingTile
	{
		int tile_x;
		int tile_y;
		bool intersects_visible_rect;
		qreal distance_sq;
	};
	std::vector<MissingTile> missing_tiles;
	auto const visible_center_x = (visible_rect.left() + visible_rect.right()) * 0.5;
	auto const visible_center_y = (visible_rect.top() + visible_rect.bottom()) * 0.5;

	for (int ty = tile_y_min; ty <= tile_y_max; ++ty)
	{
		for (int tx = tile_x_min; tx <= tile_x_max; ++tx)
		{
			auto const key = tileKey(tx, ty, subsampling);
			auto it = tile_cache.constFind(key);
			auto const source_w = std::min(block_w, tiled_raster_size.width() - tx * block_w);
			auto const source_h = std::min(block_h, tiled_raster_size.height() - ty * block_h);
			if (it != tile_cache.constEnd())
			{
				noteTileAccess(key);
				// Draw cached tile at its position in template pixel space.
				auto const px = tx * block_w - half_w;
				auto const py = ty * block_h - half_h;
				painter->drawImage(QRectF(px, py, source_w, source_h), it.value());
#ifdef Mapper_DEVELOPMENT_BUILD
				++tiles_drawn;
#endif
			}
			else
			{
				if (auto const* fallback = findBestCachedTile(tx, ty, subsampling))
				{
					auto const px = tx * block_w - half_w;
					auto const py = ty * block_h - half_h;
					painter->drawImage(QRectF(px, py, source_w, source_h), *fallback);
#ifdef Mapper_DEVELOPMENT_BUILD
					++tiles_drawn;
#endif
				}
				auto const tile_center_x = tx * block_w - half_w + source_w * 0.5;
				auto const tile_center_y = ty * block_h - half_h + source_h * 0.5;
				auto const dx = tile_center_x - visible_center_x;
				auto const dy = tile_center_y - visible_center_y;
				auto const tile_left = tx * block_w - half_w;
				auto const tile_top = ty * block_h - half_h;
				auto const intersects_visible_rect =
					tile_left < visible_rect.right()
					&& tile_left + source_w > visible_rect.left()
					&& tile_top < visible_rect.bottom()
					&& tile_top + source_h > visible_rect.top();
				missing_tiles.push_back(MissingTile{ tx, ty, intersects_visible_rect, dx * dx + dy * dy });
#ifdef Mapper_DEVELOPMENT_BUILD
				++tiles_requested;
#endif
			}
		}
	}

	std::sort(missing_tiles.begin(), missing_tiles.end(), [](auto const& lhs, auto const& rhs) {
		if (lhs.intersects_visible_rect != rhs.intersects_visible_rect)
			return lhs.intersects_visible_rect > rhs.intersects_visible_rect;
		if (lhs.distance_sq != rhs.distance_sq)
			return lhs.distance_sq < rhs.distance_sq;
		return std::tie(lhs.tile_y, lhs.tile_x) < std::tie(rhs.tile_y, rhs.tile_x);
	});

	for (auto it = missing_tiles.rbegin(); it != missing_tiles.rend(); ++it)
				requestTile(it->tile_x, it->tile_y, subsampling, request_generation);

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

void GdalTemplate::tileWorkerLoop(GDALDatasetH worker_dataset)
{
	while (true)
	{
		TileRequest tile_request;
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			queue_cv.wait(lock, [this] {
				return !tile_queue.isEmpty() || worker_stop.load();
			});
			if (worker_stop.load())
				return;
			tile_request = tile_queue.dequeue();
		}

		auto const block_w = tiled_raster_info.block_size.width();
		auto const block_h = tiled_raster_info.block_size.height();

		// Determine the pixel rectangle for this tile, clamped to raster bounds.
		int const px = tile_request.tile_x * block_w;
		int const py = tile_request.tile_y * block_h;
		int const read_w = std::min(block_w, tiled_raster_size.width() - px);
		int const read_h = std::min(block_h, tiled_raster_size.height() - py);

		if (read_w <= 0 || read_h <= 0)
			continue;

		auto const subsampling = std::max(1, tile_request.subsampling);
		auto const output_w = std::max(1, (read_w + subsampling - 1) / subsampling);
		auto const output_h = std::max(1, (read_h + subsampling - 1) / subsampling);

		QImage tile(output_w, output_h, tiled_raster_info.image_format);
		if (tile.isNull())
			continue;

		tile.fill(Qt::white);

		GDALRasterIOExtraArg extra_arg;
		INIT_RASTERIO_EXTRA_ARG(extra_arg);
		RasterIoCancellationContext cancellation_context{
			&worker_stop,
			&active_request_generation,
			tile_request.generation
		};
		extra_arg.pfnProgress = tileReadProgress;
		extra_arg.pProgressData = &cancellation_context;

		CPLErrorReset();
		auto result = GDALDatasetRasterIOEx(
			worker_dataset, GF_Read,
			px, py, read_w, read_h,
			tile.bits() + tiled_raster_info.band_offset, output_w, output_h,
			GDT_Byte,
			tiled_raster_info.bands.count(), tiled_raster_info.bands.data(),
			tiled_raster_info.pixel_space, tile.bytesPerLine(),
			tiled_raster_info.band_space,
			&extra_arg);

		if (result >= CE_Warning)
		{
			auto const canceled = worker_stop.load(std::memory_order_relaxed)
			                      || active_request_generation.load(std::memory_order_relaxed) != tile_request.generation;
			if (!canceled)
			{
				qDebug("GdalTemplate: Tile read failed at (%d,%d): %s",
				       tile_request.tile_x, tile_request.tile_y,
				       CPLGetLastErrorMsg());
			}

			auto const key = tileKey(tile_request.tile_x, tile_request.tile_y, subsampling);
			QMetaObject::invokeMethod(
				const_cast<GdalTemplate*>(this),
				[this, key, tile_x = tile_request.tile_x, tile_y = tile_request.tile_y]() {
					onTileLoadFailed(key, tile_x, tile_y);
				},
				Qt::QueuedConnection);
			continue;
		}

		if (shouldDebugGdalTemplate() && debug_tile_logs_remaining > 0)
		{
			--debug_tile_logs_remaining;
			qInfo().noquote()
				<< "GdalTemplate tileRead"
				<< "path=" << template_path
				<< "tile=(" << tile_request.tile_x << "," << tile_request.tile_y << ")"
				<< "pixel_window=(" << px << "," << py << "," << read_w << "," << read_h << ")"
				<< "subsampling=" << subsampling
				<< "output_size=(" << output_w << "," << output_h << ")";
		}

		tiled_raster_info.postprocessing(tile);

		auto const key = tileKey(tile_request.tile_x, tile_request.tile_y, subsampling);

		// Post the loaded tile to the UI thread.
		QMetaObject::invokeMethod(
			const_cast<GdalTemplate*>(this),
			[this, key, tile_x = tile_request.tile_x, tile_y = tile_request.tile_y, tile = std::move(tile)]() mutable {
				onTileLoaded(key, tile_x, tile_y, std::move(tile));
			},
			Qt::QueuedConnection);
	}
}


void GdalTemplate::requestTile(int tile_x, int tile_y, int subsampling, quint64 generation) const
{
	auto const key = tileKey(tile_x, tile_y, subsampling);
	if (loading_tiles.contains(key))
		return;

	loading_tiles.insert(key);
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		tile_queue.prepend(TileRequest{ tile_x, tile_y, subsampling, generation });
	}
	queue_cv.notify_one();
}


void GdalTemplate::onTileLoaded(quint64 key, int tile_x, int tile_y, QImage tile_image)
{
	auto const existing = tile_cache.constFind(key);
	if (existing != tile_cache.constEnd())
		tile_cache_bytes -= tileByteCost(existing.value());

	tile_cache.insert(key, std::move(tile_image));
	tile_cache_bytes += tileByteCost(tile_cache.value(key));
	noteTileAccess(key);
	evictCachedTilesToBudget();
	loading_tiles.remove(key);

	// Only repaint the loaded tile footprint instead of the full template.
	markTileAreaDirty(tile_x, tile_y);
}


void GdalTemplate::onTileLoadFailed(quint64 key, int tile_x, int tile_y)
{
	loading_tiles.remove(key);
	markTileAreaDirty(tile_x, tile_y);
}


// static
quint64 GdalTemplate::tileKey(int tile_x, int tile_y, int subsampling)
{
	int level = 0;
	for (int factor = std::max(1, subsampling); factor > 1; factor >>= 1)
		++level;
	return (quint64(level & 0xFF) << 56)
	       | (quint64(quint32(tile_x) & 0x0FFFFFFF) << 28)
	       | quint64(quint32(tile_y) & 0x0FFFFFFF);
}


const QImage* GdalTemplate::findBestCachedTile(int tile_x, int tile_y, int desired_subsampling) const
{
	auto const max_subsampling = std::max(1, std::min(tiled_raster_info.block_size.width(),
	                                                   tiled_raster_info.block_size.height()));

	for (int delta = 1; delta <= max_subsampling; delta <<= 1)
	{
		if (desired_subsampling / delta >= 1)
		{
			auto it = tile_cache.constFind(tileKey(tile_x, tile_y, desired_subsampling / delta));
			if (it != tile_cache.constEnd())
			{
				noteTileAccess(it.key());
				return &it.value();
			}
		}

		auto coarser = desired_subsampling * delta;
		if (coarser <= max_subsampling)
		{
			auto it = tile_cache.constFind(tileKey(tile_x, tile_y, coarser));
			if (it != tile_cache.constEnd())
			{
				noteTileAccess(it.key());
				return &it.value();
			}
		}
	}

	return nullptr;
}


// static
int GdalTemplate::chooseTileSubsampling(double scale, const QSize& block_size)
{
	return tileSubsamplingForScale(scale, block_size);
}


void GdalTemplate::noteTileAccess(quint64 key) const
{
	tile_cache_access.insert(key, ++tile_cache_access_counter);
}


void GdalTemplate::evictCachedTilesToBudget()
{
	while (tile_cache_bytes > tile_cache_budget_bytes && !tile_cache.isEmpty())
	{
		bool found = false;
		quint64 oldest_key = 0;
		quint64 oldest_access = std::numeric_limits<quint64>::max();

		for (auto it = tile_cache.constBegin(); it != tile_cache.constEnd(); ++it)
		{
			auto const access = tile_cache_access.value(it.key(), 0);
			if (!found || access < oldest_access)
			{
				found = true;
				oldest_key = it.key();
				oldest_access = access;
			}
		}

		if (!found)
			break;

		auto it = tile_cache.find(oldest_key);
		if (it == tile_cache.end())
			break;

		tile_cache_bytes -= tileByteCost(it.value());
		tile_cache.erase(it);
		tile_cache_access.remove(oldest_key);
	}
}


void GdalTemplate::markTileAreaDirty(int tile_x, int tile_y) const
{
	auto const block_w = tiled_raster_info.block_size.width();
	auto const block_h = tiled_raster_info.block_size.height();
	auto const source_w = std::min(block_w, tiled_raster_size.width() - tile_x * block_w);
	auto const source_h = std::min(block_h, tiled_raster_size.height() - tile_y * block_h);
	if (source_w <= 0 || source_h <= 0)
		return;

	auto const half_w = tiled_raster_size.width() * 0.5;
	auto const half_h = tiled_raster_size.height() * 0.5;
	auto const template_rect = QRectF(tile_x * block_w - half_w,
	                                  tile_y * block_h - half_h,
	                                  source_w,
	                                  source_h);

	QRectF map_rect;
	rectIncludeSafe(map_rect, templateToMap(template_rect.topLeft()));
	rectInclude(map_rect, templateToMap(template_rect.topRight()));
	rectInclude(map_rect, templateToMap(template_rect.bottomLeft()));
	rectInclude(map_rect, templateToMap(template_rect.bottomRight()));
	map->setTemplateAreaDirty(const_cast<GdalTemplate*>(this), map_rect, getTemplateBoundingBoxPixelBorder());
}


void GdalTemplate::discardQueuedTileRequests() const
{
	std::lock_guard<std::mutex> lock(queue_mutex);
	while (!tile_queue.isEmpty())
	{
		auto const request = tile_queue.dequeue();
		loading_tiles.remove(tileKey(request.tile_x, request.tile_y, request.subsampling));
	}
}


quint64 GdalTemplate::beginRequestGeneration(const RequestWindow& request_window) const
{
	if (request_window == current_request_window)
		return active_request_generation.load(std::memory_order_relaxed);

	active_subsampling.store(request_window.subsampling, std::memory_order_relaxed);
	current_request_window = request_window;
	auto const generation = active_request_generation.fetch_add(1, std::memory_order_relaxed) + 1;
	discardQueuedTileRequests();
	return generation;
}


void GdalTemplate::setupTiledGeoreferencing()
{
	if (!isGeoreferencingUsable())
	{
		qWarning("%s must not be called with incomplete georeferencing", Q_FUNC_INFO);
		return;
	}

	// Reuse the normal georeferencing path so mixed-CRS rasters are
	// reprojected through the map CRS. getTemplateExtent() provides the tiled
	// raster dimensions, so this no longer depends on a full in-memory image.
	calculateGeoreferencing();

	if (shouldDebugGdalTemplate())
	{
		auto const bbox = calculateTemplateBoundingBox();
		qInfo().noquote()
			<< "GdalTemplate setupTiledGeoreferencing"
			<< "path=" << template_path
			<< "raster_size=" << tiled_raster_size
			<< "block_size=" << tiled_raster_info.block_size
			<< "crs=" << available_georef.effective.crs_spec
			<< "pixel_to_world=" << available_georef.effective.transform.pixel_to_world
			<< "scale=(" << transform.template_scale_x << "," << transform.template_scale_y << ")"
			<< "translation=(" << transform.template_x / 1000.0 << "," << transform.template_y / 1000.0 << ")"
			<< "rotation=" << transform.template_rotation
			<< "bbox=" << bbox;
	}
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
