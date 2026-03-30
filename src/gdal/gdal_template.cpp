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

#include <Qt>
#include <QtGlobal>
#include <QByteArray>
#include <QChar>
#include <QDebug>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QMetaObject>
#include <QPainter>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QString>
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

QByteArray findExportFormat(const QString& filename)
{
	auto const formats = QImageWriter::supportedImageFormats();
	for (auto const& format : formats)
	{
		if (format.length() >= 3
		    && filename.endsWith(QLatin1String(format.constData()), Qt::CaseInsensitive))
			return format;
	}
	return {};
}

int tileSubsamplingForScale(double scale, const QSize& block_size)
{
	if (!(scale > 0.0) || block_size.isEmpty())
		return 1;

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

int tileIndexForRasterCoord(double raster_coord, qint64 tile_span)
{
	if (tile_span <= 0)
		return 0;

	return int(std::floor(raster_coord / double(tile_span)));
}

int maxTileIndexForRasterExtent(int raster_extent, qint64 tile_span)
{
	if (raster_extent <= 0 || tile_span <= 0)
		return -1;

	return int((qint64(raster_extent) - 1) / tile_span);
}

QRect sourceRectForTileImpl(const QSize& raster_size,
                            const QSize& block_size,
                            int tile_x,
                            int tile_y,
                            int subsampling)
{
	auto const safe_subsampling = std::max(1, subsampling);
	auto const tile_span_w = qint64(block_size.width()) * safe_subsampling;
	auto const tile_span_h = qint64(block_size.height()) * safe_subsampling;
	auto const px0 = qint64(tile_x) * tile_span_w;
	auto const py0 = qint64(tile_y) * tile_span_h;
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
{}

GdalTemplate::~GdalTemplate()
{
	shutdownTiledSource();
}

GdalTemplate* GdalTemplate::duplicate() const
{
	auto* copy = new GdalTemplate(*this);
	if (template_state == Loaded && isTiledSource())
		copy->loadTemplateFileImpl();
	return copy;
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
	auto georef_options = findAvailableGeoreferencing(reader.readGeoTransform());
	auto const can_use_tiled_source = !raster_info.block_size.isEmpty()
	                                  && raster_info.image_format != QImage::Format_Invalid
	                                  && (!georef_options.effective.transform.source.isEmpty()
	                                      || property(applyCornerPassPointsProperty()).toBool());
	if (can_use_tiled_source)
	{
		GdalManager();
		CPLErrorReset();
		tiled_dataset = GDALOpen(template_path.toUtf8(), GA_ReadOnly);
		if (!tiled_dataset)
		{
			setErrorString(tr("Failed to open tiled raster: %1")
			               .arg(QString::fromUtf8(CPLGetLastErrorMsg())));
			return false;
		}

		CPLErrorReset();
		worker_dataset = GDALOpen(template_path.toUtf8(), GA_ReadOnly);
		if (!worker_dataset)
		{
			setErrorString(tr("Failed to open tiled raster worker: %1")
			               .arg(QString::fromUtf8(CPLGetLastErrorMsg())));
			shutdownTiledSource();
			return false;
		}

		if (raster_info.image_format == QImage::Format_Indexed8
		    && !raster_info.bands.empty())
		{
			auto color_table = reader.readColorTable(raster_info.bands.front());
			raster_info.postprocessing = [color_table](QImage& img) {
				img.setColorTable(color_table);
			};
		}

		tiled_raster_info = raster_info;
		tiled_raster_size = raster_info.size;
		available_georef = std::move(georef_options);

		if (!is_georeferenced && isGeoreferencingUsable())
			is_georeferenced = true;

		if (is_georeferenced)
		{
			if (!isGeoreferencingUsable())
			{
				setErrorString(::OpenOrienteering::TemplateImage::tr("Georeferencing not found"));
				shutdownTiledSource();
				return false;
			}

			setupTiledGeoreferencing();
		}
		else if (property(applyCornerPassPointsProperty()).toBool())
		{
			if (!applyCornerPassPoints())
			{
				shutdownTiledSource();
				return false;
			}
		}

		worker_stop = false;
		worker_thread = std::thread(&GdalTemplate::tileWorkerLoop, this);
		return true;
	}

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

	available_georef = std::move(georef_options);
	if (is_georeferenced)
	{
		if (!isGeoreferencingUsable())
		{
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

	drawable = !findExportFormat(template_path).isEmpty();
	return true;
}


bool GdalTemplate::postLoadSetup(QWidget* dialog_parent, bool& out_center_in_view)
{
	if (!isTiledSource())
		return TemplateImage::postLoadSetup(dialog_parent, out_center_in_view);

	if (is_georeferenced || property(applyCornerPassPointsProperty()).toBool())
	{
		out_center_in_view = false;
		return true;
	}

	setErrorString(::OpenOrienteering::TemplateImage::tr("Georeferencing not found"));
	return false;
}


void GdalTemplate::unloadTemplateFileImpl()
{
	shutdownTiledSource();
	TemplateImage::unloadTemplateFileImpl();
}


void GdalTemplate::updateRenderContext(const ViewRenderContext& context)
{
	if (!isTiledSource() || !context.on_screen)
		return;

	auto const scale = std::max(getTemplateScaleX(), getTemplateScaleY()) * context.view_zoom;
	auto const subsampling = chooseTileSubsampling(Util::mmToPixelPhysical(scale), tiled_raster_info.block_size);
	auto const window = tileWindowForMapRect(context.visible_map_rect, subsampling);
	auto const replace_pending_tiles = !(window == wanted_window);
	wanted_window = window;
	queueWantedTiles(window, replace_pending_tiles);
}


void GdalTemplate::drawTemplate(QPainter* painter, const QRectF& clip_rect, double scale, bool on_screen, qreal opacity) const
{
	if (!isTiledSource())
	{
		TemplateImage::drawTemplate(painter, clip_rect, scale, on_screen, opacity);
		return;
	}

	applyTemplateTransform(painter);
	painter->setRenderHint(QPainter::SmoothPixmapTransform);
	painter->setOpacity(opacity);

	int subsampling = wanted_window.subsampling;
	if (!on_screen)
	{
		auto effective_scale = scale;
		auto dpi = painter->device()->physicalDpiX();
		if (!dpi)
			dpi = painter->device()->logicalDpiX();
		if (dpi > 0)
			effective_scale *= dpi / 25.4;
		subsampling = chooseTileSubsampling(effective_scale, tiled_raster_info.block_size);
	}

	auto const window = tileWindowForMapRect(clip_rect, subsampling);
	auto const half_w = tiled_raster_size.width() * 0.5;
	auto const half_h = tiled_raster_size.height() * 0.5;

	for (int ty = window.tile_y_min; ty <= window.tile_y_max; ++ty)
	{
		for (int tx = window.tile_x_min; tx <= window.tile_x_max; ++tx)
		{
			auto const src = sourceRectForTile(tiled_raster_size, tiled_raster_info.block_size, tx, ty, subsampling);
			if (src.isEmpty())
				continue;

			auto const dest_rect = QRectF(src.x() - half_w, src.y() - half_h, src.width(), src.height());
			auto const key = tileKey(tx, ty, subsampling);

			auto it = tile_cache.constFind(key);
			if (it != tile_cache.constEnd())
			{
				painter->drawImage(dest_rect, it.value().image);
				continue;
			}

			if (!on_screen)
			{
				auto tile = readTileImage(tiled_dataset, tx, ty, subsampling);
				if (!tile.isNull())
					painter->drawImage(dest_rect, tile);
			}
		}
	}

	painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
}


QRectF GdalTemplate::getTemplateExtent() const
{
	if (!isTiledSource())
		return TemplateImage::getTemplateExtent();

	auto const w = tiled_raster_size.width();
	auto const h = tiled_raster_size.height();
	return QRectF(-w * 0.5, -h * 0.5, w, h);
}


void GdalTemplate::shutdownTiledSource()
{
	if (worker_thread.joinable())
	{
		worker_stop = true;
		queue_cv.notify_all();
		worker_thread.join();
	}
	worker_stop = false;

	if (worker_dataset)
	{
		GDALClose(worker_dataset);
		worker_dataset = nullptr;
	}
	if (tiled_dataset)
	{
		GDALClose(tiled_dataset);
		tiled_dataset = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		tile_queue.clear();
		queued_tiles.clear();
		loading_tiles.clear();
	}

	cache_order.clear();
	tile_cache.clear();
	tile_cache_bytes = 0;
	tiled_raster_info = {};
	tiled_raster_size = {};
	wanted_window = {};
}


void GdalTemplate::tileWorkerLoop()
{
	while (true)
	{
		TileRequest tile_request;
		GdalTileKey key;
		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			queue_cv.wait(lock, [this] {
				return worker_stop.load(std::memory_order_relaxed) || !tile_queue.empty();
			});
			if (worker_stop.load(std::memory_order_relaxed))
				return;

			tile_request = tile_queue.front();
			tile_queue.pop_front();
			key = tileKey(tile_request.tile_x, tile_request.tile_y, tile_request.subsampling);
			queued_tiles.remove(key);
			loading_tiles.insert(key);
		}

		auto tile = readTileImage(worker_dataset, tile_request.tile_x, tile_request.tile_y, tile_request.subsampling);
		if (tile.isNull())
		{
			QMetaObject::invokeMethod(
				this,
				[this, key]() {
					onTileLoadFailed(key);
				},
				Qt::QueuedConnection);
			continue;
		}

		QMetaObject::invokeMethod(
			this,
			[this, key, tile = std::move(tile)]() mutable {
				onTileLoaded(key, std::move(tile));
			},
			Qt::QueuedConnection);
	}
}


void GdalTemplate::queueWantedTiles(const TileWindow& window, bool replace_pending_tiles)
{
	struct MissingTile
	{
		GdalTileKey key;
		double dist_sq = 0.0;
	};

	std::vector<MissingTile> missing_tiles;
	auto const half_w = tiled_raster_size.width() * 0.5;
	auto const half_h = tiled_raster_size.height() * 0.5;
	auto const visible_center_x = 0.5 * (window.tile_x_min + window.tile_x_max + 1)
	                              * qint64(tiled_raster_info.block_size.width()) * window.subsampling
	                              - half_w;
	auto const visible_center_y = 0.5 * (window.tile_y_min + window.tile_y_max + 1)
	                              * qint64(tiled_raster_info.block_size.height()) * window.subsampling
	                              - half_h;

	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		if (replace_pending_tiles)
		{
			tile_queue.clear();
			queued_tiles.clear();
		}

		if (window.isEmpty())
			return;

		for (int ty = window.tile_y_min; ty <= window.tile_y_max; ++ty)
		{
			for (int tx = window.tile_x_min; tx <= window.tile_x_max; ++tx)
			{
				auto const key = tileKey(tx, ty, window.subsampling);
				if (tile_cache.contains(key) || loading_tiles.contains(key) || queued_tiles.contains(key))
					continue;

				auto const src = sourceRectForTile(tiled_raster_size, tiled_raster_info.block_size, tx, ty, window.subsampling);
				if (src.isEmpty())
					continue;

				auto const cx = (src.x() - half_w) + 0.5 * src.width() - visible_center_x;
				auto const cy = (src.y() - half_h) + 0.5 * src.height() - visible_center_y;
				missing_tiles.push_back({ key, cx * cx + cy * cy });
			}
		}
	}

	std::sort(missing_tiles.begin(), missing_tiles.end(), [](const MissingTile& lhs, const MissingTile& rhs) {
		return lhs.dist_sq < rhs.dist_sq;
	});

	if (missing_tiles.empty())
		return;

	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		for (auto const& missing : missing_tiles)
		{
			tile_queue.push_back({ missing.key.tile_x, missing.key.tile_y, missing.key.subsampling });
			queued_tiles.insert(missing.key);
		}
	}
	queue_cv.notify_one();
}


QImage GdalTemplate::readTileImage(GDALDatasetH dataset, int tile_x, int tile_y, int subsampling) const
{
	if (!dataset)
		return {};

	auto const src = sourceRectForTile(tiled_raster_size, tiled_raster_info.block_size, tile_x, tile_y, subsampling);
	if (src.isEmpty())
		return {};

	auto const safe_subsampling = std::max(1, subsampling);
	auto const output_w = std::max(1, (src.width() + safe_subsampling - 1) / safe_subsampling);
	auto const output_h = std::max(1, (src.height() + safe_subsampling - 1) / safe_subsampling);

	QImage tile(output_w, output_h, tiled_raster_info.image_format);
	if (tile.isNull())
		return {};

	tile.fill(Qt::white);

	GDALRasterIOExtraArg extra_arg;
	INIT_RASTERIO_EXTRA_ARG(extra_arg);

	CPLErrorReset();
	auto result = GDALDatasetRasterIOEx(
		dataset, GF_Read,
		src.x(), src.y(), src.width(), src.height(),
		tile.bits() + tiled_raster_info.band_offset, output_w, output_h,
		GDT_Byte,
		tiled_raster_info.bands.count(), tiled_raster_info.bands.data(),
		tiled_raster_info.pixel_space, tile.bytesPerLine(),
		tiled_raster_info.band_space,
		&extra_arg);
	if (result >= CE_Warning)
		return {};

	tiled_raster_info.postprocessing(tile);
	return tile;
}


void GdalTemplate::onTileLoaded(const GdalTileKey& key, QImage tile_image)
{
	auto it = tile_cache.find(key);
	if (it != tile_cache.end())
	{
		tile_cache_bytes -= tileByteCost(it.value().image);
		it.value().image = std::move(tile_image);
		tile_cache_bytes += tileByteCost(it.value().image);
	}
	else
	{
		cache_order.push_front(key);
		it = tile_cache.insert(key, CachedTileEntry{ std::move(tile_image) });
		tile_cache_bytes += tileByteCost(it.value().image);
	}

	evictCachedTilesToBudget();
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		loading_tiles.remove(key);
	}

	markTileAreaDirty(key.tile_x, key.tile_y, key.subsampling);
}


void GdalTemplate::onTileLoadFailed(const GdalTileKey& key)
{
	std::lock_guard<std::mutex> lock(queue_mutex);
	loading_tiles.remove(key);
}


void GdalTemplate::evictCachedTilesToBudget()
{
	while (tile_cache_bytes > tile_cache_budget_bytes && !cache_order.empty())
	{
		auto const oldest_key = cache_order.back();
		cache_order.pop_back();
		auto it = tile_cache.find(oldest_key);
		if (it == tile_cache.end())
			continue;

		tile_cache_bytes -= tileByteCost(it.value().image);
		tile_cache.erase(it);
	}
}


void GdalTemplate::markTileAreaDirty(int tile_x, int tile_y, int subsampling)
{
	auto const src = sourceRectForTile(tiled_raster_size, tiled_raster_info.block_size, tile_x, tile_y, std::max(1, subsampling));
	if (src.isEmpty())
		return;

	auto const half_w = tiled_raster_size.width() * 0.5;
	auto const half_h = tiled_raster_size.height() * 0.5;
	auto const template_rect = QRectF(src.x() - half_w, src.y() - half_h, src.width(), src.height());

	QRectF map_rect;
	rectIncludeSafe(map_rect, templateToMap(template_rect.topLeft()));
	rectInclude(map_rect, templateToMap(template_rect.topRight()));
	rectInclude(map_rect, templateToMap(template_rect.bottomLeft()));
	rectInclude(map_rect, templateToMap(template_rect.bottomRight()));
	map->setTemplateAreaDirty(this, map_rect, getTemplateBoundingBoxPixelBorder());
}


// static
bool GdalTemplate::readTmsTileOrigin(const QString& template_path, QPoint* origin_tile)
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


GdalTemplate::TileWindow GdalTemplate::tileWindowForMapRect(const QRectF& map_rect, int subsampling) const
{
	TileWindow window;
	if (!isTiledSource() || map_rect.isEmpty())
		return window;

	QRectF visible_rect;
	rectIncludeSafe(visible_rect, mapToTemplate(MapCoordF(map_rect.topLeft())));
	rectIncludeSafe(visible_rect, mapToTemplate(MapCoordF(map_rect.topRight())));
	rectIncludeSafe(visible_rect, mapToTemplate(MapCoordF(map_rect.bottomLeft())));
	rectIncludeSafe(visible_rect, mapToTemplate(MapCoordF(map_rect.bottomRight())));

	auto const half_w = tiled_raster_size.width() * 0.5;
	auto const half_h = tiled_raster_size.height() * 0.5;
	auto const step_w = qint64(tiled_raster_info.block_size.width()) * std::max(1, subsampling);
	auto const step_h = qint64(tiled_raster_info.block_size.height()) * std::max(1, subsampling);

	window.tile_x_min = std::max(0, tileIndexForRasterCoord(visible_rect.left() + half_w, step_w));
	window.tile_y_min = std::max(0, tileIndexForRasterCoord(visible_rect.top() + half_h, step_h));
	window.tile_x_max = std::min(maxTileIndexForRasterExtent(tiled_raster_size.width(), step_w),
	                             tileIndexForRasterCoord(visible_rect.right() + half_w, step_w));
	window.tile_y_max = std::min(maxTileIndexForRasterExtent(tiled_raster_size.height(), step_h),
	                             tileIndexForRasterCoord(visible_rect.bottom() + half_h, step_h));
	window.subsampling = std::max(1, subsampling);

	return window;
}


// static
GdalTileKey GdalTemplate::tileKey(int tile_x, int tile_y, int subsampling)
{
	return { tile_x, tile_y, std::max(1, subsampling) };
}


// static
int GdalTemplate::chooseTileSubsampling(double scale, const QSize& block_size)
{
	return tileSubsamplingForScale(scale, block_size);
}


int GdalTemplate::capSubsamplingForTmsAlignment(int subsampling) const
{
	subsampling = std::max(1, subsampling);
	if (!has_tiled_origin_tile)
		return subsampling;

	while (subsampling > 1
	       && (tiled_origin_tile.x() % subsampling != 0
	           || tiled_origin_tile.y() % subsampling != 0))
	{
		subsampling >>= 1;
	}
	return subsampling;
}


// static
QRect GdalTemplate::sourceRectForTile(const QSize& raster_size,
                                      const QSize& block_size,
                                      int tile_x,
                                      int tile_y,
                                      int subsampling)
{
	return sourceRectForTileImpl(raster_size, block_size, tile_x, tile_y, subsampling);
}


void GdalTemplate::setupTiledGeoreferencing()
{
	if (!isGeoreferencingUsable())
	{
		qWarning("%s must not be called with incomplete georeferencing", Q_FUNC_INFO);
		return;
	}

	georef = std::make_unique<Georeferencing>();
	georef->setProjectedCRS(QString{}, available_georef.effective.crs_spec);
	georef->setTransformationDirectly(available_georef.effective.transform.pixel_to_world);
	if (map->getGeoreferencing().getState() == Georeferencing::Geospatial)
		updateTiledPosFromGeoreferencing();
}


void GdalTemplate::updateTiledPosFromGeoreferencing()
{
	bool ok;
	MapCoordF top_left = map->getGeoreferencing().toMapCoordF(georef.get(), MapCoordF(0.0, 0.0), &ok);
	if (!ok)
	{
		qDebug("%s failed", Q_FUNC_INFO);
		return;
	}
	MapCoordF top_right = map->getGeoreferencing().toMapCoordF(georef.get(), MapCoordF(tiled_raster_size.width(), 0.0), &ok);
	if (!ok)
	{
		qDebug("%s failed", Q_FUNC_INFO);
		return;
	}
	MapCoordF bottom_left = map->getGeoreferencing().toMapCoordF(georef.get(), MapCoordF(0.0, tiled_raster_size.height()), &ok);
	if (!ok)
	{
		qDebug("%s failed", Q_FUNC_INFO);
		return;
	}

	PassPointList pp_list;

	PassPoint pp;
	pp.src_coords = MapCoordF(-0.5 * tiled_raster_size.width(), -0.5 * tiled_raster_size.height());
	pp.dest_coords = top_left;
	pp_list.push_back(pp);
	pp.src_coords = MapCoordF(0.5 * tiled_raster_size.width(), -0.5 * tiled_raster_size.height());
	pp.dest_coords = top_right;
	pp_list.push_back(pp);
	pp.src_coords = MapCoordF(-0.5 * tiled_raster_size.width(), 0.5 * tiled_raster_size.height());
	pp.dest_coords = bottom_left;
	pp_list.push_back(pp);

	QTransform q_transform;
	if (!pp_list.estimateNonIsometricSimilarityTransform(&q_transform))
	{
		qDebug("%s failed", Q_FUNC_INFO);
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
