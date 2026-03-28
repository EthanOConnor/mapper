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
#include <limits>
#include <memory>
#include <tuple>

#include <Qt>
#include <QtGlobal>
#include <QByteArray>
#include <QChar>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QList>
#include <QMetaObject>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QVariant>
#include <QXmlStreamReader>

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

	// Bias slightly toward coarser overview levels so interactive panning
	// keeps tiles filled when a source pixel would already render close to
	// a screen pixel anyway.
	constexpr double target_screen_pixels_per_source_pixel = 1.5;
	auto const max_subsampling = std::max(1, std::min(block_size.width(), block_size.height()));
	int subsampling = 1;
	while (subsampling * 2 <= max_subsampling
	       && scale * (subsampling * 2) <= target_screen_pixels_per_source_pixel)
	{
		subsampling *= 2;
	}
	return subsampling;
}

qsizetype tileByteCost(const QImage& tile)
{
	return qsizetype(tile.bytesPerLine()) * tile.height();
}

int positiveModulo(qint64 value, int divisor)
{
	if (divisor <= 0)
		return 0;

	auto result = int(value % divisor);
	if (result < 0)
		result += divisor;
	return result;
}

/**
 * Restrict overview subsampling to levels where the TMS tile origin aligns.
 *
 * For TMS sources, chooseOverviewReadPlan() reads directly from a GDAL
 * overview band and maps coordinates with a linear pixel ratio.  That
 * mapping is only correct when the DataWindow's tile origin is an exact
 * multiple of the overview's downsample factor; otherwise the overview
 * band's pixel registration is offset from the base band, shifting the
 * rendered content.  (For example, TileY 366160 % 32 = 16, which
 * produces a 4096-pixel -- 25% -- southward shift on a 16384-pixel
 * raster.)
 *
 * This function finds the largest power-of-two subsampling at which the
 * origin IS aligned, so chooseOverviewReadPlan() selects only safe
 * overview levels.  When no aligned level exists (subsampling falls to 1),
 * the caller skips the direct-overview path and GDALDatasetRasterIOEx
 * handles overview selection internally.
 */
int alignedOverviewSubsampling(const QPoint& origin_tile, bool has_origin_tile, int requested_subsampling)
{
	auto aligned_subsampling = std::max(1, requested_subsampling);
	if (!has_origin_tile)
		return aligned_subsampling;

	while (aligned_subsampling > 1
	       && (positiveModulo(origin_tile.x(), aligned_subsampling) != 0
	           || positiveModulo(origin_tile.y(), aligned_subsampling) != 0))
	{
		aligned_subsampling >>= 1;
	}

	return aligned_subsampling;
}

bool readTmsTileOrigin(const QString& template_path, QPoint* origin_tile)
{
	QFile xml_file(template_path);
	if (!xml_file.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;

	QXmlStreamReader xml(&xml_file);
	bool is_tms_service = false;
	bool in_data_window = false;
	qint64 tile_x = 0;
	qint64 tile_y = 0;
	bool have_tile_x = false;
	bool have_tile_y = false;

	while (!xml.atEnd())
	{
		xml.readNext();
		if (!xml.isStartElement())
		{
			if (in_data_window && xml.isEndElement() && xml.name() == QLatin1String("DataWindow"))
				in_data_window = false;
			continue;
		}

		if (xml.name() == QLatin1String("Service"))
		{
			auto const service_name = xml.attributes().value(QStringLiteral("name"));
			is_tms_service = service_name.compare(QLatin1String("TMS"), Qt::CaseInsensitive) == 0;
			continue;
		}

		if (xml.name() == QLatin1String("DataWindow"))
		{
			in_data_window = true;
			continue;
		}

		if (!is_tms_service || !in_data_window)
			continue;

		if (xml.name() == QLatin1String("TileX"))
		{
			bool ok = false;
			auto const value = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed().toLongLong(&ok);
			if (!ok)
				return false;
			tile_x = value;
			have_tile_x = true;
		}
		else if (xml.name() == QLatin1String("TileY"))
		{
			bool ok = false;
			auto const value = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed().toLongLong(&ok);
			if (!ok)
				return false;
			tile_y = value;
			have_tile_y = true;
		}
	}

	if (xml.hasError() || !is_tms_service || !have_tile_x || !have_tile_y)
		return false;

	if (origin_tile)
		*origin_tile = QPoint(int(tile_x), int(tile_y));
	return true;
}

int tileIndexForRasterCoord(double raster_coord, qint64 tile_span, int alignment_offset_pixels)
{
	if (tile_span <= 0)
		return 0;

	return int(std::floor((raster_coord + alignment_offset_pixels) / double(tile_span)));
}

int maxTileIndexForRasterExtent(int raster_extent, qint64 tile_span, int alignment_offset_pixels)
{
	if (raster_extent <= 0 || tile_span <= 0)
		return -1;

	return int((qint64(raster_extent) - 1 + alignment_offset_pixels) / tile_span);
}

QRect sourceRectForTile(const QSize& raster_size,
                        const QSize& block_size,
                        const QPoint& alignment_offset_pixels,
                        int tile_x,
                        int tile_y,
                        int subsampling)
{
	auto const safe_subsampling = std::max(1, subsampling);
	auto const tile_span_w = qint64(block_size.width()) * safe_subsampling;
	auto const tile_span_h = qint64(block_size.height()) * safe_subsampling;
	auto const px0 = qint64(tile_x) * tile_span_w - alignment_offset_pixels.x();
	auto const py0 = qint64(tile_y) * tile_span_h - alignment_offset_pixels.y();
	auto const px1 = px0 + tile_span_w;
	auto const py1 = py0 + tile_span_h;
	auto const px = std::max<qint64>(0, px0);
	auto const py = std::max<qint64>(0, py0);
	auto const end_x = std::min<qint64>(raster_size.width(), px1);
	auto const end_y = std::min<qint64>(raster_size.height(), py1);
	if (px >= end_x || py >= end_y)
		return QRect();

	return QRect(int(px), int(py), int(end_x - px), int(end_y - py));
}

QSize outputSizeForSourceRect(const QRect& source_rect, int subsampling)
{
	auto const safe_subsampling = std::max(1, subsampling);
	if (source_rect.isEmpty())
		return QSize();

	return QSize((source_rect.width() + safe_subsampling - 1) / safe_subsampling,
	             (source_rect.height() + safe_subsampling - 1) / safe_subsampling);
}

QRectF templateRectForTileRect(const QRect& raster_rect, const QSize& raster_size)
{
	auto const half_w = raster_size.width() * 0.5;
	auto const half_h = raster_size.height() * 0.5;
	return QRectF(raster_rect.x() - half_w,
	              raster_rect.y() - half_h,
	              raster_rect.width(),
	              raster_rect.height());
}

QRectF sourceRectWithinCachedTile(const QRect& desired_rect, const QRect& cached_rect, const QSize& cached_image_size)
{
	if (cached_rect.isEmpty() || cached_image_size.isEmpty())
		return QRectF();

	auto const scale_x = cached_image_size.width() / double(cached_rect.width());
	auto const scale_y = cached_image_size.height() / double(cached_rect.height());
	return QRectF((desired_rect.x() - cached_rect.x()) * scale_x,
	              (desired_rect.y() - cached_rect.y()) * scale_y,
	              desired_rect.width() * scale_x,
	              desired_rect.height() * scale_y);
}

struct RasterIoCancellationContext
{
	const GdalTemplate* template_instance = nullptr;
	std::atomic<bool>* stop_flag = nullptr;
	int tile_x = 0;
	int tile_y = 0;
	int subsampling = 1;
};

struct OverviewReadPlan
{
	int overview_index = -1;
	QRect overview_rect;
};

OverviewReadPlan chooseOverviewReadPlan(GDALDatasetH dataset,
                                        int band_number,
                                        const QSize& base_raster_size,
                                        const QRect& source_rect,
                                        int requested_subsampling)
{
	OverviewReadPlan plan;
	if (!dataset || band_number <= 0 || source_rect.isEmpty() || requested_subsampling <= 1)
		return plan;

	auto* base_band = GDALGetRasterBand(dataset, band_number);
	if (!base_band)
		return plan;

	auto const overview_count = GDALGetOverviewCount(base_band);
	if (overview_count <= 0)
		return plan;

	auto best_error = std::numeric_limits<double>::infinity();
	QSize best_overview_size;
	for (int i = 0; i < overview_count; ++i)
	{
		auto* overview_band = GDALGetOverview(base_band, i);
		if (!overview_band)
			continue;

		auto const overview_size = QSize(GDALGetRasterBandXSize(overview_band),
		                                 GDALGetRasterBandYSize(overview_band));
		if (overview_size.isEmpty())
			continue;

		auto const downsample = std::max(
			base_raster_size.width() / double(overview_size.width()),
			base_raster_size.height() / double(overview_size.height()));
		if (!(downsample > 1.0))
			continue;

		auto const error = std::abs(std::log(downsample / requested_subsampling));
		if (error < best_error)
		{
			best_error = error;
			plan.overview_index = i;
			best_overview_size = overview_size;
		}
	}

	if (plan.overview_index < 0)
		return plan;
	if (best_error > std::log(1.5))
	{
		plan.overview_index = -1;
		return plan;
	}

	auto const x0 = int((qint64(source_rect.x()) * best_overview_size.width()) / base_raster_size.width());
	auto const y0 = int((qint64(source_rect.y()) * best_overview_size.height()) / base_raster_size.height());
	auto const x1 = int((qint64(source_rect.x() + source_rect.width()) * best_overview_size.width()) / base_raster_size.width());
	auto const y1 = int((qint64(source_rect.y() + source_rect.height()) * best_overview_size.height()) / base_raster_size.height());
	plan.overview_rect = QRect(x0, y0, x1 - x0, y1 - y0);
	if (plan.overview_rect.isEmpty())
	{
		plan.overview_index = -1;
		return plan;
	}

	return plan;
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
		has_tiled_origin_tile = readTmsTileOrigin(template_path, &tiled_origin_tile);

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
		if (!CPLGetConfigOption("GDAL_MAX_CONNECTIONS", nullptr))
		{
			auto const connections = QByteArray::number(std::max(1, gdal_max_connections));
			CPLSetConfigOption("GDAL_MAX_CONNECTIONS", connections.constData());
		}
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
	tile_cache_lru.clear();
	tile_cache.clear();
	tile_cache_bytes = 0;
	active_subsampling.store(0, std::memory_order_relaxed);
	active_request_generation.store(0, std::memory_order_relaxed);
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		current_request_window = {};
		tile_queue.clear();
		queued_tiles.clear();
		loading_tiles.clear();
	}

	tiled_raster_info = {};
	tiled_raster_size = {};
	tiled_origin_tile = {};
	has_tiled_origin_tile = false;

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
	auto const tile_span_w = qint64(block_w) * subsampling;
	auto const tile_span_h = qint64(block_h) * subsampling;
	auto const alignment_offset = sourceAlignmentOffsetPixels(subsampling);

	// Determine the tile grid range that covers the visible area, plus 1 tile overscan.
	int tile_x_min = tileIndexForRasterCoord(visible_rect.left() + half_w, tile_span_w, alignment_offset.x()) - 1;
	int tile_y_min = tileIndexForRasterCoord(visible_rect.top() + half_h, tile_span_h, alignment_offset.y()) - 1;
	int tile_x_max = tileIndexForRasterCoord(visible_rect.right() + half_w, tile_span_w, alignment_offset.x()) + 1;
	int tile_y_max = tileIndexForRasterCoord(visible_rect.bottom() + half_h, tile_span_h, alignment_offset.y()) + 1;
	int visible_tile_x_min = tileIndexForRasterCoord(visible_rect.left() + half_w, tile_span_w, alignment_offset.x());
	int visible_tile_y_min = tileIndexForRasterCoord(visible_rect.top() + half_h, tile_span_h, alignment_offset.y());
	int visible_tile_x_max = tileIndexForRasterCoord(visible_rect.right() + half_w, tile_span_w, alignment_offset.x());
	int visible_tile_y_max = tileIndexForRasterCoord(visible_rect.bottom() + half_h, tile_span_h, alignment_offset.y());

	// Clamp to raster bounds.
	int const max_tile_x = maxTileIndexForRasterExtent(tiled_raster_size.width(), tile_span_w, alignment_offset.x());
	int const max_tile_y = maxTileIndexForRasterExtent(tiled_raster_size.height(), tile_span_h, alignment_offset.y());
	tile_x_min = std::max(tile_x_min, 0);
	tile_y_min = std::max(tile_y_min, 0);
	tile_x_max = std::min(tile_x_max, max_tile_x);
	tile_y_max = std::min(tile_y_max, max_tile_y);
	visible_tile_x_min = std::max(0, std::min(visible_tile_x_min, max_tile_x));
	visible_tile_y_min = std::max(0, std::min(visible_tile_y_min, max_tile_y));
	visible_tile_x_max = std::max(0, std::min(visible_tile_x_max, max_tile_x));
	visible_tile_y_max = std::max(0, std::min(visible_tile_y_max, max_tile_y));

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

	auto const request_window = RequestWindow{
		tile_x_min, tile_y_min, tile_x_max, tile_y_max, subsampling,
		visible_tile_x_min, visible_tile_y_min, visible_tile_x_max, visible_tile_y_max
	};
	auto const request_generation = beginRequestGeneration(request_window);

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
			auto const source_rect = sourceRectForTile(
				tiled_raster_size,
				tiled_raster_info.block_size,
				alignment_offset,
				tx,
				ty,
				subsampling);
			if (source_rect.isEmpty())
				continue;

			auto const template_rect = templateRectForTileRect(source_rect, tiled_raster_size);
			auto const intersects_visible_rect =
				template_rect.left() < visible_rect.right()
				&& template_rect.right() > visible_rect.left()
				&& template_rect.top() < visible_rect.bottom()
				&& template_rect.bottom() > visible_rect.top();
			if (it != tile_cache.constEnd())
			{
				noteTileAccess(key);
				painter->drawImage(template_rect, it.value().image);
#ifdef Mapper_DEVELOPMENT_BUILD
				++tiles_drawn;
#endif
			}
			else
			{
				QRectF fallback_source_rect;
				if (auto const* fallback = findBestCachedTile(tx, ty, subsampling, &fallback_source_rect))
				{
					painter->drawImage(template_rect, *fallback, fallback_source_rect);
#ifdef Mapper_DEVELOPMENT_BUILD
					++tiles_drawn;
#endif
				}
				auto const tile_center_x = template_rect.left() + template_rect.width() * 0.5;
				auto const tile_center_y = template_rect.top() + template_rect.height() * 0.5;
				auto const dx = tile_center_x - visible_center_x;
				auto const dy = tile_center_y - visible_center_y;
				missing_tiles.push_back(MissingTile{ tx, ty, intersects_visible_rect, dx * dx + dy * dy });
#ifdef Mapper_DEVELOPMENT_BUILD
				++tiles_requested;
#endif
			}
		}
	}

	std::sort(missing_tiles.begin(), missing_tiles.end(), [](auto const& lhs, auto const& rhs) {
		if (lhs.intersects_visible_rect != rhs.intersects_visible_rect)
			return lhs.intersects_visible_rect < rhs.intersects_visible_rect;
		if (lhs.distance_sq != rhs.distance_sq)
			return lhs.distance_sq > rhs.distance_sq;
		return std::tie(lhs.tile_y, lhs.tile_x) > std::tie(rhs.tile_y, rhs.tile_x);
	});

	for (auto const& tile : missing_tiles)
		requestTile(tile.tile_x, tile.tile_y, subsampling, request_generation);

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
		quint64 tile_request_key = 0;
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			queue_cv.wait(lock, [this] {
				return !tile_queue.empty() || worker_stop.load();
			});
			if (worker_stop.load())
				return;
			auto queue_it = tile_queue.begin();
			tile_request = *queue_it;
			tile_request_key = tileKey(tile_request.tile_x, tile_request.tile_y, tile_request.subsampling);
			queued_tiles.remove(tile_request_key);
			tile_queue.erase(queue_it);
			if (!tileMatchesRequestWindow(tile_request.tile_x, tile_request.tile_y, tile_request.subsampling, current_request_window))
				continue;
			if (!tileIntersectsVisibleWindow(tile_request.tile_x, tile_request.tile_y, tile_request.subsampling, current_request_window))
			{
				bool have_queued_visible_tiles = false;
				for (auto const& queued_request : tile_queue)
				{
					if (tileIntersectsVisibleWindow(
					        queued_request.tile_x, queued_request.tile_y, queued_request.subsampling, current_request_window))
					{
						have_queued_visible_tiles = true;
						break;
					}
				}
				if (have_queued_visible_tiles)
					continue;
			}
			loading_tiles.insert(tile_request_key);
		}

		auto const alignment_offset = sourceAlignmentOffsetPixels(tile_request.subsampling);
		auto const source_rect = sourceRectForTile(
			tiled_raster_size,
			tiled_raster_info.block_size,
			alignment_offset,
			tile_request.tile_x,
			tile_request.tile_y,
			tile_request.subsampling);
		auto const overview_subsampling = alignedOverviewSubsampling(
			tiled_origin_tile,
			has_tiled_origin_tile,
			tile_request.subsampling);
		auto const desired_output_size = outputSizeForSourceRect(source_rect, tile_request.subsampling);
		auto const overview_plan = chooseOverviewReadPlan(
			worker_dataset,
			tiled_raster_info.bands.empty() ? 0 : tiled_raster_info.bands.front(),
			tiled_raster_size,
			source_rect,
			overview_subsampling);
		auto const use_direct_overview = overview_plan.overview_index >= 0;
		auto const output_size = desired_output_size;
		int const px = source_rect.x();
		int const py = source_rect.y();
		int const read_w = source_rect.width();
		int const read_h = source_rect.height();

		if (read_w <= 0 || read_h <= 0)
		{
			std::lock_guard<std::mutex> lock(queue_mutex);
			loading_tiles.remove(tile_request_key);
			continue;
		}

		auto const subsampling = std::max(1, tile_request.subsampling);
		auto const output_w = output_size.width();
		auto const output_h = output_size.height();

		QImage tile(output_w, output_h, tiled_raster_info.image_format);
		if (tile.isNull())
		{
			std::lock_guard<std::mutex> lock(queue_mutex);
			loading_tiles.remove(tile_request_key);
			continue;
		}

		tile.fill(Qt::white);

		GDALRasterIOExtraArg extra_arg;
		INIT_RASTERIO_EXTRA_ARG(extra_arg);
		RasterIoCancellationContext cancellation_context{
				this,
				&worker_stop,
				tile_request.tile_x,
				tile_request.tile_y,
				tile_request.subsampling
			};
		extra_arg.pfnProgress = &GdalTemplate::tileReadProgress;
		extra_arg.pProgressData = &cancellation_context;

		CPLErrorReset();
		CPLErr result = CE_None;
		if (use_direct_overview)
		{
			for (int band_offset = 0; band_offset < tiled_raster_info.bands.count(); ++band_offset)
			{
				auto* base_band = GDALGetRasterBand(worker_dataset, tiled_raster_info.bands[band_offset]);
				auto* overview_band = base_band ? GDALGetOverview(base_band, overview_plan.overview_index) : nullptr;
				if (!overview_band)
				{
					result = CE_Failure;
					break;
				}

				result = GDALRasterIOEx(
					overview_band, GF_Read,
					overview_plan.overview_rect.x(), overview_plan.overview_rect.y(),
					overview_plan.overview_rect.width(), overview_plan.overview_rect.height(),
					tile.bits() + tiled_raster_info.band_offset + band_offset * tiled_raster_info.band_space,
					output_w, output_h,
					GDT_Byte,
					tiled_raster_info.pixel_space,
					tile.bytesPerLine(),
					&extra_arg);
				if (result >= CE_Warning)
					break;
			}
		}
		else
		{
			result = GDALDatasetRasterIOEx(
				worker_dataset, GF_Read,
				px, py, read_w, read_h,
				tile.bits() + tiled_raster_info.band_offset, output_w, output_h,
				GDT_Byte,
				tiled_raster_info.bands.count(), tiled_raster_info.bands.data(),
				tiled_raster_info.pixel_space, tile.bytesPerLine(),
				tiled_raster_info.band_space,
				&extra_arg);
		}

		if (result >= CE_Warning)
		{
			auto const canceled = worker_stop.load(std::memory_order_relaxed)
			                      || !isTileRelevantToCurrentRequestWindow(tile_request.tile_x, tile_request.tile_y, tile_request.subsampling);
			if (!canceled)
			{
				qDebug("GdalTemplate: Tile read failed at (%d,%d): %s",
				       tile_request.tile_x, tile_request.tile_y,
				       CPLGetLastErrorMsg());
			}

			auto const key = tileKey(tile_request.tile_x, tile_request.tile_y, tile_request.subsampling);
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

		auto const key = tileKey(tile_request.tile_x, tile_request.tile_y, tile_request.subsampling);

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
	bool notify_worker = false;
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		if (loading_tiles.contains(key))
			return;

		auto queued_it = queued_tiles.find(key);
		if (queued_it != queued_tiles.end())
		{
			queued_it.value()->generation = generation;
			if (queued_it.value() != tile_queue.begin())
				tile_queue.splice(tile_queue.begin(), tile_queue, queued_it.value());
			return;
		}

		tile_queue.push_front(TileRequest{ tile_x, tile_y, subsampling, generation });
		queued_tiles.insert(key, tile_queue.begin());
		notify_worker = true;
	}
	if (notify_worker)
		queue_cv.notify_one();
}


void GdalTemplate::onTileLoaded(quint64 key, int tile_x, int tile_y, QImage tile_image)
{
	auto const subsampling = tileSubsamplingFromKey(key);
	auto it = tile_cache.find(key);
	if (it != tile_cache.end())
	{
		tile_cache_bytes -= tileByteCost(it.value().image);
		it.value().image = std::move(tile_image);
		tile_cache_bytes += tileByteCost(it.value().image);
	}
	else
	{
		tile_cache_lru.push_front(key);
		it = tile_cache.insert(key, CachedTileEntry{ std::move(tile_image), tile_cache_lru.begin() });
		tile_cache_bytes += tileByteCost(it.value().image);
	}

	noteTileAccess(key);
	evictCachedTilesToBudget();
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		loading_tiles.remove(key);
	}

	// Only repaint the loaded tile footprint instead of the full template.
	markTileAreaDirty(tile_x, tile_y, subsampling);
}


void GdalTemplate::onTileLoadFailed(quint64 key, int tile_x, int tile_y)
{
	auto const subsampling = tileSubsamplingFromKey(key);
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		loading_tiles.remove(key);
	}
	markTileAreaDirty(tile_x, tile_y, subsampling);
}


// static
quint64 GdalTemplate::tileKey(int tile_x, int tile_y, int subsampling)
{
	return (quint64(subsampling & 0x0FFF) << 52)
	       | (quint64(quint32(tile_x) & 0x03FFFFFF) << 26)
	       | quint64(quint32(tile_y) & 0x03FFFFFF);
}


// static
int GdalTemplate::tileXFromKey(quint64 key)
{
	return int((key >> 26) & 0x03FFFFFF);
}


// static
int GdalTemplate::tileYFromKey(quint64 key)
{
	return int(key & 0x03FFFFFF);
}


// static
int GdalTemplate::tileSubsamplingFromKey(quint64 key)
{
	return std::max(1, int((key >> 52) & 0x0FFF));
}


// static
bool GdalTemplate::tileMatchesRequestWindow(int tile_x, int tile_y, int subsampling, const RequestWindow& request_window)
{
	return request_window.subsampling == subsampling
	       && tile_x >= request_window.tile_x_min
	       && tile_x <= request_window.tile_x_max
	       && tile_y >= request_window.tile_y_min
	       && tile_y <= request_window.tile_y_max;
}


// static
bool GdalTemplate::tileIntersectsVisibleWindow(int tile_x, int tile_y, int subsampling, const RequestWindow& request_window)
{
	auto visible_tile_x_min = request_window.visible_tile_x_min;
	auto visible_tile_y_min = request_window.visible_tile_y_min;
	auto visible_tile_x_max = request_window.visible_tile_x_max;
	auto visible_tile_y_max = request_window.visible_tile_y_max;
	if (visible_tile_x_max < visible_tile_x_min || visible_tile_y_max < visible_tile_y_min)
	{
		visible_tile_x_min = request_window.tile_x_min;
		visible_tile_y_min = request_window.tile_y_min;
		visible_tile_x_max = request_window.tile_x_max;
		visible_tile_y_max = request_window.tile_y_max;
	}

	return request_window.subsampling == subsampling
	       && tile_x >= visible_tile_x_min
	       && tile_x <= visible_tile_x_max
	       && tile_y >= visible_tile_y_min
	       && tile_y <= visible_tile_y_max;
}


// static
int GdalTemplate::tileReadProgress(double /*complete*/, const char* /*message*/, void* user_data)
{
	auto* context = static_cast<RasterIoCancellationContext*>(user_data);
	if (!context)
		return true;

	if (context->stop_flag && context->stop_flag->load(std::memory_order_relaxed))
		return false;

	if (context->template_instance
	    && !context->template_instance->shouldContinueTileRequest(
	           context->tile_x, context->tile_y, context->subsampling))
	{
		return false;
	}

	return true;
}


const QImage* GdalTemplate::findBestCachedTile(int tile_x, int tile_y, int subsampling, QRectF* source_rect) const
{
	auto const desired_rect = sourceRectForTile(
		tiled_raster_size,
		tiled_raster_info.block_size,
		sourceAlignmentOffsetPixels(subsampling),
		tile_x,
		tile_y,
		subsampling);
	if (desired_rect.isEmpty())
		return nullptr;

	auto const desired_center_x = desired_rect.x() + desired_rect.width() * 0.5;
	auto const desired_center_y = desired_rect.y() + desired_rect.height() * 0.5;
	auto const max_subsampling = std::max(1, std::min(tiled_raster_info.block_size.width(),
	                                                   tiled_raster_info.block_size.height()));
	for (int coarser_subsampling = subsampling * 2;
	     coarser_subsampling <= max_subsampling;
	     coarser_subsampling <<= 1)
	{
		auto const coarser_alignment_offset = sourceAlignmentOffsetPixels(coarser_subsampling);
		auto const coarser_tile_span_w = qint64(tiled_raster_info.block_size.width()) * coarser_subsampling;
		auto const coarser_tile_span_h = qint64(tiled_raster_info.block_size.height()) * coarser_subsampling;
		auto const coarser_tile_x = std::max(
			0,
			std::min(
				tileIndexForRasterCoord(desired_center_x, coarser_tile_span_w, coarser_alignment_offset.x()),
				std::max(0, maxTileIndexForRasterExtent(tiled_raster_size.width(), coarser_tile_span_w, coarser_alignment_offset.x()))));
		auto const coarser_tile_y = std::max(
			0,
			std::min(
				tileIndexForRasterCoord(desired_center_y, coarser_tile_span_h, coarser_alignment_offset.y()),
				std::max(0, maxTileIndexForRasterExtent(tiled_raster_size.height(), coarser_tile_span_h, coarser_alignment_offset.y()))));
		auto it = tile_cache.constFind(tileKey(coarser_tile_x, coarser_tile_y, coarser_subsampling));
		if (it == tile_cache.constEnd())
			continue;

		auto const cached_rect = sourceRectForTile(
			tiled_raster_size,
			tiled_raster_info.block_size,
			coarser_alignment_offset,
			coarser_tile_x,
			coarser_tile_y,
			coarser_subsampling);
		if (!cached_rect.contains(desired_rect))
			continue;

		noteTileAccess(it.key());
		if (source_rect)
			*source_rect = sourceRectWithinCachedTile(desired_rect, cached_rect, it.value().image.size());
		return &it.value().image;
	}

	return nullptr;
}


// static
int GdalTemplate::chooseTileSubsampling(double scale, const QSize& block_size)
{
	return tileSubsamplingForScale(scale, block_size);
}


QPoint GdalTemplate::sourceAlignmentOffsetPixels(int subsampling) const
{
	if (!has_tiled_origin_tile || tiled_raster_info.block_size.isEmpty())
		return {};

	auto const safe_subsampling = std::max(1, subsampling);
	return QPoint(
		positiveModulo(tiled_origin_tile.x(), safe_subsampling) * tiled_raster_info.block_size.width(),
		positiveModulo(tiled_origin_tile.y(), safe_subsampling) * tiled_raster_info.block_size.height());
}


void GdalTemplate::noteTileAccess(quint64 key) const
{
	auto it = tile_cache.find(key);
	if (it == tile_cache.end())
		return;

	if (it.value().lru_it != tile_cache_lru.begin())
		tile_cache_lru.splice(tile_cache_lru.begin(), tile_cache_lru, it.value().lru_it);
}


void GdalTemplate::evictCachedTilesToBudget()
{
	while (tile_cache_bytes > tile_cache_budget_bytes && !tile_cache_lru.empty())
	{
		auto const oldest_key = tile_cache_lru.back();
		tile_cache_lru.pop_back();
		auto it = tile_cache.find(oldest_key);
		if (it == tile_cache.end())
			continue;

		tile_cache_bytes -= tileByteCost(it.value().image);
		tile_cache.erase(it);
	}
}


void GdalTemplate::markTileAreaDirty(int tile_x, int tile_y, int subsampling) const
{
	auto const source_rect = sourceRectForTile(
		tiled_raster_size,
		tiled_raster_info.block_size,
		sourceAlignmentOffsetPixels(subsampling),
		tile_x,
		tile_y,
		subsampling);
	if (source_rect.isEmpty())
		return;

	auto const template_rect = templateRectForTileRect(source_rect, tiled_raster_size);

	QRectF map_rect;
	rectIncludeSafe(map_rect, templateToMap(template_rect.topLeft()));
	rectInclude(map_rect, templateToMap(template_rect.topRight()));
	rectInclude(map_rect, templateToMap(template_rect.bottomLeft()));
	rectInclude(map_rect, templateToMap(template_rect.bottomRight()));
	map->setTemplateAreaDirty(const_cast<GdalTemplate*>(this), map_rect, getTemplateBoundingBoxPixelBorder());
}


bool GdalTemplate::isTileRelevantToCurrentRequestWindow(int tile_x, int tile_y, int subsampling) const
{
	std::lock_guard<std::mutex> lock(queue_mutex);
	return tileMatchesRequestWindow(tile_x, tile_y, subsampling, current_request_window);
}


bool GdalTemplate::shouldContinueTileRequest(int tile_x, int tile_y, int subsampling) const
{
	std::lock_guard<std::mutex> lock(queue_mutex);
	if (!tileMatchesRequestWindow(tile_x, tile_y, subsampling, current_request_window))
		return false;
	if (tileIntersectsVisibleWindow(tile_x, tile_y, subsampling, current_request_window))
		return true;

	for (auto const& queued_request : tile_queue)
	{
		if (tileIntersectsVisibleWindow(
		        queued_request.tile_x, queued_request.tile_y, queued_request.subsampling, current_request_window))
		{
			return false;
		}
	}

	return true;
}


void GdalTemplate::reconcileTileRequests(quint64 generation, const RequestWindow& request_window) const
{
	std::lock_guard<std::mutex> lock(queue_mutex);
	current_request_window = request_window;

	for (auto it = tile_queue.begin(); it != tile_queue.end(); )
	{
		auto const key = tileKey(it->tile_x, it->tile_y, it->subsampling);
		if (!tileMatchesRequestWindow(it->tile_x, it->tile_y, it->subsampling, request_window))
		{
			queued_tiles.remove(key);
			it = tile_queue.erase(it);
			continue;
		}

		it->generation = generation;
		++it;
	}

	QList<quint64> stale_loading_tiles;
	for (auto key : loading_tiles)
	{
		if (!tileMatchesRequestWindow(tileXFromKey(key), tileYFromKey(key), tileSubsamplingFromKey(key), request_window))
			stale_loading_tiles.push_back(key);
	}
	for (auto key : stale_loading_tiles)
		loading_tiles.remove(key);
}


quint64 GdalTemplate::beginRequestGeneration(const RequestWindow& request_window) const
{
	if (request_window == current_request_window)
		return active_request_generation.load(std::memory_order_relaxed);

	active_subsampling.store(request_window.subsampling, std::memory_order_relaxed);
	auto const generation = active_request_generation.fetch_add(1, std::memory_order_relaxed) + 1;
	reconcileTileRequests(generation, request_window);
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
