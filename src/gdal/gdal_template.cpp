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
#include <climits>
#include <iterator>

#include <Qt>
#include <QtGlobal>
#include <QByteArray>
#include <QChar>
#include <QDebug>
#include <QFile>
#include <QImageReader>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QThread>
#include <QTimer>
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

constexpr int max_screen_subsampling = 64;
constexpr qsizetype max_queued_screen_tiles = 64;
// Bound per-view work even when the visible exact-resolution set cannot fit
// the cache. The effective limit is reduced for larger decoded tile formats.
constexpr qsizetype max_attempted_screen_tiles = 512;

int tileSubsamplingForScale(double scale, const QSize& block_size)
{
	if (!(scale > 0.0) || block_size.isEmpty())
		return 1;

	auto const max_subsampling = std::max(1, std::min(block_size.width(), block_size.height()));
	auto const desired_subsampling = 1.0 / scale;
	int subsampling = 1;
	while (subsampling * 2 <= max_subsampling
	       && desired_subsampling >= 1.5 * subsampling)
	{
		subsampling *= 2;
	}
	return subsampling;
}

int tileCacheCost(const QImage& tile)
{
	auto const bytes = qsizetype(tile.bytesPerLine()) * tile.height();
	return int(std::min<qsizetype>(INT_MAX, (bytes + 1023) / 1024));
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

QRect decodedSourceRectForTileImpl(const QSize& raster_size,
	                               const QSize& block_size,
	                               int tile_x,
	                               int tile_y,
	                               int subsampling)
{
	auto const core = sourceRectForTileImpl(
		raster_size, block_size, tile_x, tile_y, subsampling
	);
	if (core.isEmpty())
		return {};

	auto const gutter = std::max(1, subsampling);
	return core.adjusted(-gutter, -gutter, gutter, gutter)
	           .intersected(QRect(QPoint(), raster_size));
}

struct TileReadCancellation
{
	const RasterResourceManager::CancellationToken* token = nullptr;
};

int continueCurrentTileRead(double, const char*, void* data)
{
	auto const* cancellation = static_cast<const TileReadCancellation*>(data);
	return !cancellation || !cancellation->token
	       || !cancellation->token->isCancelled();
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
{
	tile_cache.setMaxCost(128 * 1024);
}

GdalTemplate::GdalTemplate(const GdalTemplate& proto)
: TemplateImage(proto)
{
	tile_cache.setMaxCost(128 * 1024);
}

GdalTemplate::~GdalTemplate()
{
	shutdownTiledSource();
}

GdalTemplate* GdalTemplate::duplicate() const
{
	auto* copy = new GdalTemplate(*this);
	if (template_state == Loaded && isTiledSource() && !copy->loadTemplateFileImpl())
		copy->setTemplateState(Invalid);
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


QSize GdalTemplate::getRasterPixelSize() const
{
	return isTiledSource() ? tiled_raster_size : TemplateImage::getRasterPixelSize();
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
	                                  && raster_info.image_format != QImage::Format_Invalid;
	if (can_use_tiled_source)
	{
		GdalManager();
		CPLErrorReset();
		auto const path = template_path.toUtf8();
		tiled_dataset = std::shared_ptr<GDALDataset>(GDALDataset::Open(
			path.constData(),
			GDAL_OF_RASTER | GDAL_OF_READONLY | GDAL_OF_THREAD_SAFE | GDAL_OF_VERBOSE_ERROR,
			nullptr, nullptr, nullptr
		), GDALDatasetUniquePtrDeleter {});
		if (!tiled_dataset)
		{
			setErrorString(tr("Failed to open tiled raster: %1")
			               .arg(QString::fromUtf8(CPLGetLastErrorMsg())));
			return false;
		}
		if (!tiled_dataset->IsThreadSafe(GDAL_OF_RASTER))
		{
			setErrorString(tr("The raster source cannot provide thread-safe reads"));
			tiled_dataset.reset();
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
		tiled_origin_tile = {};
		has_tiled_origin_tile = readTmsTileOrigin(template_path, &tiled_origin_tile);
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

		raster_owner.setConcurrencyLimit(
			RasterResourceManager::Lane::BlockingIo,
			workerCountForSource()
		);
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

	return TemplateImage::postLoadSetup(dialog_parent, out_center_in_view);
}


void GdalTemplate::unloadTemplateFileImpl()
{
	shutdownTiledSource();
	TemplateImage::unloadTemplateFileImpl();
}


void GdalTemplate::updateRenderContext(const ViewRenderContext& context)
{
	if (!isTiledSource())
		return;

	auto const scale = std::max(getTemplateScaleX(), getTemplateScaleY()) * context.view_zoom;
	auto const subsampling = chooseTiledSubsampling(Util::mmToPixelPhysical(scale));
	auto const window = screenTileWindowForMapRect(
		context.visible_map_rect, subsampling
	);
	auto const replace_pending_tiles = !(wanted_window == window)
	                                   && !wanted_window.intersects(window);
	wanted_window = window;
	queueWantedTiles(window, replace_pending_tiles);
}


QRectF GdalTemplate::getTemplateExtent() const
{
	if (!isTiledSource())
		return TemplateImage::getTemplateExtent();

	auto const w = tiled_raster_size.width();
	auto const h = tiled_raster_size.height();
	return QRectF(-w * 0.5, -h * 0.5, w, h);
}

void GdalTemplate::collectRasterTiles(const QRectF& map_clip_rect,
	                                  double scale,
	                                  bool on_screen,
	                                  QVector<RasterTemplateTile>& out) const
{
	if (!isTiledSource())
	{
		TemplateImage::collectRasterTiles(map_clip_rect, scale, on_screen, out);
		return;
	}

	auto subsampling = wanted_window.subsampling;
	if (!on_screen)
		subsampling = chooseTiledSubsampling(scale);
	auto const window = tileWindowForMapRect(map_clip_rect, subsampling);
	auto const half_w = tiled_raster_size.width() * 0.5;
	auto const half_h = tiled_raster_size.height() * 0.5;

	for (int ty = window.tile_y_min; ty <= window.tile_y_max; ++ty)
	{
		for (int tx = window.tile_x_min; tx <= window.tile_x_max; ++tx)
		{
			auto const core = sourceRectForTile(
				tiled_raster_size, tiled_raster_info.block_size, tx, ty, subsampling
			);
			if (core.isEmpty())
				continue;

			auto const target = QRectF(
				core.x() - half_w, core.y() - half_h, core.width(), core.height()
			);
			auto const key = tileKey(tx, ty, subsampling);
			if (auto const* exact = tile_cache.object(key))
			{
				auto const decoded = decodedSourceRectForTile(
					tiled_raster_size, tiled_raster_info.block_size, tx, ty, subsampling
				);
				out.push_back({
					*exact,
					target,
					sourceRectWithinCachedTile(core, decoded, exact->size()),
					quint64(exact->cacheKey()),
					false,
				});
				continue;
			}
			if (!on_screen)
			{
				auto exact = readTileImage(tx, ty, subsampling);
				if (!exact.isNull())
				{
					auto const decoded = decodedSourceRectForTile(
						tiled_raster_size, tiled_raster_info.block_size,
						tx, ty, subsampling
					);
					out.push_back({
						exact,
						target,
						sourceRectWithinCachedTile(core, decoded, exact.size()),
						quint64(exact.cacheKey()),
						false,
					});
					continue;
				}
			}

			if (on_screen)
			{
				QRectF fallback_source;
				auto const* fallback = findBestCachedTile(
					tx, ty, subsampling, &fallback_source
				);
				if (fallback)
				{
					out.push_back({
						*fallback,
						target,
						fallback_source,
						quint64(fallback->cacheKey()),
						false,
						true,
					});
					continue;
				}
			}

			out.push_back({ {}, target, {}, 0, true, false });
		}
	}
}


void GdalTemplate::shutdownTiledSource()
{
	raster_owner.invalidate();
	tiled_dataset.reset();

	queued_tiles.clear();
	failed_tiles.clear();
	attempted_window = {};
	attempted_tiles.clear();

	tile_cache.clear();
	tiled_raster_info = GdalImageReader::RasterInfo();
	tiled_raster_size = {};
	wanted_window = {};
	tiled_origin_tile = {};
	has_tiled_origin_tile = false;
}


void GdalTemplate::queueWantedTiles(const TileWindow& window, bool replace_pending_tiles)
{
	struct MissingTile
	{
		GdalTileKey key;
		double dist_sq = 0.0;
		bool fallback = false;
	};

	std::vector<MissingTile> missing_tiles;
	QSet<GdalTileKey> planned_tiles;
	auto const half_w = tiled_raster_size.width() * 0.5;
	auto const half_h = tiled_raster_size.height() * 0.5;
	auto const visible_center_x = 0.5 * (window.tile_x_min + window.tile_x_max + 1)
	                              * qint64(tiled_raster_info.block_size.width()) * window.subsampling
	                              - half_w;
	auto const visible_center_y = 0.5 * (window.tile_y_min + window.tile_y_max + 1)
	                              * qint64(tiled_raster_info.block_size.height()) * window.subsampling
	                              - half_h;

	if (replace_pending_tiles)
	{
		raster_owner.invalidate();
		queued_tiles.clear();
		failed_tiles.clear();
	}
	if (replace_pending_tiles || !(window == attempted_window))
	{
		attempted_window = window;
		attempted_tiles.clear();
	}

	if (window.isEmpty())
		return;
	auto const admission_budget = screenTileAdmissionBudget();
	if (attempted_tiles.size() >= admission_budget)
		return;

	auto planIfMissing = [this, &missing_tiles, &planned_tiles](
		const GdalTileKey& key, double dist_sq, bool fallback) {
		if (tile_cache.contains(key) || queued_tiles.contains(key)
		    || attempted_tiles.contains(key) || planned_tiles.contains(key))
		{
			return;
		}
		auto const failed = failed_tiles.constFind(key);
		if (failed != failed_tiles.cend() && !failed->retry.hasExpired())
			return;
		auto const source = sourceRectForTile(
			tiled_raster_size, tiled_raster_info.block_size,
			key.tile_x, key.tile_y, key.subsampling
		);
		if (source.isEmpty())
			return;
		missing_tiles.push_back({ key, dist_sq, fallback });
		planned_tiles.insert(key);
	};

	for (int ty = window.tile_y_min; ty <= window.tile_y_max; ++ty)
	{
		for (int tx = window.tile_x_min; tx <= window.tile_x_max; ++tx)
		{
			auto const key = tileKey(tx, ty, window.subsampling);
			if (tile_cache.contains(key))
				continue;

			auto const src = sourceRectForTile(tiled_raster_size, tiled_raster_info.block_size, tx, ty, window.subsampling);
			if (src.isEmpty())
				continue;

			auto const cx = (src.x() - half_w) + 0.5 * src.width() - visible_center_x;
			auto const cy = (src.y() - half_h) + 0.5 * src.height() - visible_center_y;
			auto const dist_sq = cx * cx + cy * cy;

			if (!findBestCachedTile(tx, ty, window.subsampling, nullptr))
			{
				auto const max_subsampling = std::max(
					1, std::min({
						tiled_raster_info.block_size.width(),
						tiled_raster_info.block_size.height(),
						max_screen_subsampling,
					})
				);
				auto const safe_subsampling = std::max(1, window.subsampling);
				int coarsest_subsampling = 1;
				while (coarsest_subsampling * 2 <= max_subsampling)
					coarsest_subsampling <<= 1;
				for (int coarser = coarsest_subsampling;
				     coarser >= safe_subsampling * 2;
				     coarser >>= 1)
				{
					if (!isTiledSubsamplingAligned(coarser))
						continue;
					auto const coarser_x = int(qint64(tx) * safe_subsampling / coarser);
					auto const coarser_y = int(qint64(ty) * safe_subsampling / coarser);
					planIfMissing(
						tileKey(coarser_x, coarser_y, coarser), dist_sq, true
					);
				}
			}

			planIfMissing(key, dist_sq, false);
		}
	}

	std::sort(
		missing_tiles.begin(), missing_tiles.end(),
		[](const MissingTile& lhs, const MissingTile& rhs) {
			if (lhs.fallback != rhs.fallback)
				return lhs.fallback;
			if (lhs.fallback && lhs.key.subsampling != rhs.key.subsampling)
				return lhs.key.subsampling > rhs.key.subsampling;
			return lhs.dist_sq < rhs.dist_sq;
		}
	);

	if (missing_tiles.empty())
		return;

	auto available_slots = std::max<qsizetype>(
		0, max_queued_screen_tiles - queued_tiles.size()
	);
	auto available_admissions = std::max<qsizetype>(
		0, admission_budget - attempted_tiles.size()
	);
	for (auto const& missing : missing_tiles)
	{
		if (available_slots == 0 || available_admissions == 0)
			break;
		auto const key = missing.key;
		if (tile_cache.contains(key) || queued_tiles.contains(key)
		    || attempted_tiles.contains(key))
			continue;
		auto const accepted = RasterResourceManager::instance().submit(
			raster_owner,
			RasterResourceManager::Lane::BlockingIo,
			missing.fallback
				? RasterResourceManager::Priority::Coverage
				: RasterResourceManager::Priority::Visible,
			this,
			[
				dataset = tiled_dataset,
				raster_info = tiled_raster_info,
				raster_size = tiled_raster_size,
				key,
				receiver = this
			](const RasterResourceManager::CancellationToken& cancellation) mutable {
				auto tile = readTileImage(
					dataset, raster_info, raster_size,
					key.tile_x, key.tile_y, key.subsampling, &cancellation
				);
				return RasterResourceManager::Completion {
					[receiver, key, tile = std::move(tile)]() mutable {
						if (tile.isNull())
							receiver->onTileLoadFailed(key);
						else
							receiver->onTileLoaded(key, std::move(tile));
					}
				};
			}
		);
		if (!accepted)
			continue;
		attempted_tiles.insert(key);
		queued_tiles.insert(key);
		--available_slots;
		--available_admissions;
	}
}


QImage GdalTemplate::readTileImage(
	int tile_x, int tile_y, int subsampling) const
{
	return readTileImage(
		tiled_dataset, tiled_raster_info, tiled_raster_size,
		tile_x, tile_y, subsampling, nullptr
	);
}

QImage GdalTemplate::readTileImage(
	const std::shared_ptr<GDALDataset>& dataset,
	const GdalImageReader::RasterInfo& raster_info,
	const QSize& raster_size,
	int tile_x,
	int tile_y,
	int subsampling,
	const RasterResourceManager::CancellationToken* cancellation)
{
	if (!dataset)
		return {};

	auto const src = decodedSourceRectForTile(
		raster_size, raster_info.block_size, tile_x, tile_y, subsampling
	);
	if (src.isEmpty())
		return {};

	auto const safe_subsampling = std::max(1, subsampling);
	auto const output_w = std::max(1, (src.width() + safe_subsampling - 1) / safe_subsampling);
	auto const output_h = std::max(1, (src.height() + safe_subsampling - 1) / safe_subsampling);

	QImage tile(output_w, output_h, raster_info.image_format);
	if (tile.isNull())
		return {};

	tile.fill(Qt::white);

	GDALRasterIOExtraArg extra_arg;
	INIT_RASTERIO_EXTRA_ARG(extra_arg);
	if (safe_subsampling > 1)
		extra_arg.eResampleAlg = GRIORA_Average;
	TileReadCancellation read_cancellation { cancellation };
	if (cancellation)
	{
		extra_arg.pfnProgress = continueCurrentTileRead;
		extra_arg.pProgressData = &read_cancellation;
	}

	CPLErrorReset();
	auto bands = raster_info.bands;
	auto result = dataset->RasterIO(
		GF_Read,
		src.x(), src.y(), src.width(), src.height(),
		tile.bits() + raster_info.band_offset, output_w, output_h,
		GDT_Byte,
		bands.count(), bands.data(),
		raster_info.pixel_space, tile.bytesPerLine(),
		raster_info.band_space,
		&extra_arg);
	if (result >= CE_Warning)
		return {};

	raster_info.postprocessing(tile);
	return tile;
}


void GdalTemplate::onTileLoaded(
	const GdalTileKey& key, QImage tile_image)
{
	queued_tiles.remove(key);
	if (!isTiledSource())
		return;

	failed_tiles.remove(key);
	auto const cost = tileCacheCost(tile_image);
	tile_cache.insert(key, new QImage(std::move(tile_image)), cost);

	markTileAreaDirty(key.tile_x, key.tile_y, key.subsampling);
	queueWantedTiles(wanted_window, false);
}


void GdalTemplate::onTileLoadFailed(
	const GdalTileKey& key)
{
	queued_tiles.remove(key);
	if (!isTiledSource())
		return;

	auto& failure = failed_tiles[key];
	failure.attempts = std::min(failure.attempts + 1, 8);
	auto const delay = std::min(30'000, 250 << (failure.attempts - 1));
	failure.retry = QDeadlineTimer(delay);

	queueWantedTiles(wanted_window, false);
	auto const expected_generation = raster_owner.generation();
	QTimer::singleShot(delay, this, [this, key, expected_generation] {
		if (!isTiledSource() || raster_owner.generation() != expected_generation)
			return;
		auto const failed = failed_tiles.constFind(key);
		if (failed == failed_tiles.cend() || !failed->retry.hasExpired())
			return;
		queueWantedTiles(wanted_window, false);
	});
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


const QImage* GdalTemplate::findBestCachedTile(int tile_x, int tile_y, int subsampling, QRectF* source_rect) const
{
	auto const desired_rect = sourceRectForTile(
		tiled_raster_size,
		tiled_raster_info.block_size,
		tile_x,
		tile_y,
		subsampling);
	if (desired_rect.isEmpty())
		return nullptr;

	auto const max_subsampling = std::max(1, std::min({
		tiled_raster_info.block_size.width(),
		tiled_raster_info.block_size.height(),
		max_screen_subsampling,
	}));
	auto const safe_subsampling = std::max(1, subsampling);
	int coarser_tile_x = tile_x / 2;
	int coarser_tile_y = tile_y / 2;
	for (int coarser_subsampling = safe_subsampling * 2;
	     coarser_subsampling <= max_subsampling;
	     coarser_subsampling <<= 1, coarser_tile_x /= 2, coarser_tile_y /= 2)
	{
		auto const* cached = tile_cache.object(
			tileKey(coarser_tile_x, coarser_tile_y, coarser_subsampling)
		);
		if (!cached)
			continue;

		auto const cached_rect = sourceRectForTile(
			tiled_raster_size,
			tiled_raster_info.block_size,
			coarser_tile_x,
			coarser_tile_y,
			coarser_subsampling);
		if (!cached_rect.contains(desired_rect))
			continue;

		if (source_rect)
		{
			auto const decoded_rect = decodedSourceRectForTile(
				tiled_raster_size, tiled_raster_info.block_size,
				coarser_tile_x, coarser_tile_y, coarser_subsampling
			);
			*source_rect = sourceRectWithinCachedTile(
				desired_rect, decoded_rect, cached->size()
			);
		}
		return cached;
	}

	return nullptr;
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


GdalTemplate::TileWindow GdalTemplate::screenTileWindowForMapRect(
	const QRectF& map_rect, int subsampling) const
{
	auto window = tileWindowForMapRect(map_rect, subsampling);
	auto const admission_budget = screenTileAdmissionBudget();
	auto const max_subsampling = std::max(1, std::min({
		tiled_raster_info.block_size.width(),
		tiled_raster_info.block_size.height(),
		max_screen_subsampling,
	}));

	while (window.tileCount() > admission_budget)
	{
		auto coarser = window.subsampling * 2;
		while (coarser <= max_subsampling
		       && !isTiledSubsamplingAligned(coarser))
		{
			coarser *= 2;
		}
		if (coarser > max_subsampling)
			return {};
		window = tileWindowForMapRect(map_rect, coarser);
	}

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


int GdalTemplate::chooseTiledSubsampling(double scale) const
{
	auto subsampling = chooseTileSubsampling(scale, tiled_raster_info.block_size);

	// GDAL WMS/TMS overview bands lose the sub-tile remainder bits of TileX
	// and TileY when the origin is not aligned with the overview factor.
	// Restrict GDAL to overview levels that keep the cropped origin aligned.
	while (subsampling > 1 && !isTiledSubsamplingAligned(subsampling))
	{
		subsampling >>= 1;
	}
	return subsampling;
}


bool GdalTemplate::isTiledSubsamplingAligned(int subsampling) const
{
	auto const safe_subsampling = std::max(1, subsampling);
	return !has_tiled_origin_tile
	       || (tiled_origin_tile.x() % safe_subsampling == 0
	           && tiled_origin_tile.y() % safe_subsampling == 0);
}


qsizetype GdalTemplate::screenTileAdmissionBudget() const
{
	auto const block_size = tiled_raster_info.block_size;
	auto const bits_per_pixel = std::max(
		8, int(QImage::toPixelFormat(tiled_raster_info.image_format).bitsPerPixel())
	);
	auto const decoded_bytes = qint64(block_size.width()) * block_size.height()
	                           * bits_per_pixel / 8;
	auto const estimated_cost = std::max<qint64>(1, (decoded_bytes + 1023) / 1024);
	auto const cache_capacity = tile_cache.maxCost() / estimated_cost;
	return std::clamp<qsizetype>(
		cache_capacity, max_queued_screen_tiles, max_attempted_screen_tiles
	);
}


int GdalTemplate::workerCountForSource() const
{
	return std::clamp(QThread::idealThreadCount(), 1, 4);
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


// static
QRect GdalTemplate::decodedSourceRectForTile(const QSize& raster_size,
	                                         const QSize& block_size,
	                                         int tile_x,
	                                         int tile_y,
	                                         int subsampling)
{
	return decodedSourceRectForTileImpl(
		raster_size, block_size, tile_x, tile_y, subsampling
	);
}


// static
QRectF GdalTemplate::sourceRectWithinCachedTile(const QRect& desired_rect,
                                                const QRect& cached_rect,
                                                const QSize& cached_image_size)
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
