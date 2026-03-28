/*
 *    Copyright 2019-2020 Kai Pastor
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

#ifndef OPENORIENTEERING_GDAL_TEMPLATE_H
#define OPENORIENTEERING_GDAL_TEMPLATE_H

#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include <vector>

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>

#include "gdal/gdal_image_reader.h"
#include "templates/template.h"
#include "templates/template_image.h"

class GdalTiledTemplateTest;
class QByteArray;
class QPainter;
class QRectF;

typedef void* GDALDatasetH;

namespace OpenOrienteering {

class Map;


/**
 * Support for geospatial raster data.
 *
 * For raster sources with a native tile/block structure (WMS, WMTS, TMS,
 * MBTiles, Cloud Optimized GeoTIFF), this class loads tiles on demand in
 * a background thread and renders from a tile cache. For non-tiled sources,
 * it delegates to TemplateImage which loads the full image into memory.
 */
class GdalTemplate : public TemplateImage
{
public:
	static bool canRead(const QString& path);

	static const std::vector<QByteArray>& supportedExtensions();

	static const char* applyCornerPassPointsProperty();

	GdalTemplate(const QString& path, Map* map);
	~GdalTemplate() override;

protected:
	GdalTemplate(const GdalTemplate& proto);
	GdalTemplate* duplicate() const override;

public:
	const char* getTemplateType() const override;

	LookupResult tryToFindTemplateFile(const QString& map_path) override;

	bool fileExists() const override;

	/**
	 * Returns true if this template uses tiled/block-based loading.
	 */
	bool isTiledSource() const;

protected:
	bool loadTemplateFileImpl() override;
	void unloadTemplateFileImpl() override;

	void drawTemplate(QPainter* painter, const QRectF& clip_rect, double scale, bool on_screen, qreal opacity) const override;
	QRectF getTemplateExtent() const override;

	bool applyCornerPassPoints();

	/**
	 * Set up georeferencing for a tiled source without allocating a
	 * full-size QImage. Uses tiled_raster_size for the corner math
	 * that TemplateImage::updatePosFromGeoreferencing() normally does
	 * via image.width()/height().
	 */
	void setupTiledGeoreferencing();

private:
	struct RequestWindow
	{
		int tile_x_min = 0;
		int tile_y_min = 0;
		int tile_x_max = -1;
		int tile_y_max = -1;
		int subsampling = 1;
		int visible_tile_x_min = 0;
		int visible_tile_y_min = 0;
		int visible_tile_x_max = -1;
		int visible_tile_y_max = -1;

		RequestWindow() = default;

		RequestWindow(int tile_x_min,
		              int tile_y_min,
		              int tile_x_max,
		              int tile_y_max,
		              int subsampling = 1,
		              int visible_tile_x_min = 0,
		              int visible_tile_y_min = 0,
		              int visible_tile_x_max = -1,
		              int visible_tile_y_max = -1)
		: tile_x_min(tile_x_min),
		  tile_y_min(tile_y_min),
		  tile_x_max(tile_x_max),
		  tile_y_max(tile_y_max),
		  subsampling(subsampling),
		  visible_tile_x_min(visible_tile_x_min),
		  visible_tile_y_min(visible_tile_y_min),
		  visible_tile_x_max(visible_tile_x_max),
		  visible_tile_y_max(visible_tile_y_max)
		{}

		bool operator==(const RequestWindow& other) const
		{
			return tile_x_min == other.tile_x_min
			       && tile_y_min == other.tile_y_min
			       && tile_x_max == other.tile_x_max
			       && tile_y_max == other.tile_y_max
			       && subsampling == other.subsampling
			       && visible_tile_x_min == other.visible_tile_x_min
			       && visible_tile_y_min == other.visible_tile_y_min
			       && visible_tile_x_max == other.visible_tile_x_max
			       && visible_tile_y_max == other.visible_tile_y_max;
		}
	};

	struct TileRequest
	{
		int tile_x = 0;
		int tile_y = 0;
		int subsampling = 1;
		quint64 generation = 0;
	};

	using LruOrder = std::list<quint64>;
	using TileQueue = std::list<TileRequest>;

	struct CachedTileEntry
	{
		QImage image;
		LruOrder::iterator lru_it;
	};

	/**
	 * Background thread function for loading tiles from GDAL.
	 */
	void tileWorkerLoop(GDALDatasetH worker_dataset);

	/**
	 * Queue a tile for background loading. Safe to call from const context
	 * (drawTemplate) because tile loading is a cache operation, not a
	 * semantic state change.
	 */
	void requestTile(int tile_x, int tile_y, int subsampling, quint64 generation) const;

	/**
	 * Called on the UI thread when a tile has been loaded by the worker.
	 */
	void onTileLoaded(quint64 key, int tile_x, int tile_y, QImage tile_image);
	void onTileLoadFailed(quint64 key, int tile_x, int tile_y);

	/**
	 * Compute a cache key from tile grid coordinates.
	 */
	static quint64 tileKey(int tile_x, int tile_y, int subsampling);
	static int tileXFromKey(quint64 key);
	static int tileYFromKey(quint64 key);
	static int tileSubsamplingFromKey(quint64 key);

	const QImage* findBestCachedTile(int tile_x, int tile_y, int subsampling, QRectF* source_rect) const;

	static int chooseTileSubsampling(double scale, const QSize& block_size);
	static bool tileMatchesRequestWindow(int tile_x, int tile_y, int subsampling, const RequestWindow& request_window);
	static bool tileIntersectsVisibleWindow(int tile_x, int tile_y, int subsampling, const RequestWindow& request_window);
	static int tileReadProgress(double complete, const char* message, void* user_data);

	void noteTileAccess(quint64 key) const;
	void evictCachedTilesToBudget();
	void markTileAreaDirty(int tile_x, int tile_y, int subsampling) const;
	void reconcileTileRequests(quint64 generation, const RequestWindow& request_window) const;
	quint64 beginRequestGeneration(const RequestWindow& request_window) const;
	bool isTileRelevantToCurrentRequestWindow(int tile_x, int tile_y, int subsampling) const;
	bool shouldContinueTileRequest(int tile_x, int tile_y, int subsampling) const;
	QPoint sourceAlignmentOffsetPixels(int subsampling) const;

	// Tiled source state (null/empty when using the non-tiled path)
	GDALDatasetH tiled_dataset = nullptr;
	std::vector<GDALDatasetH> worker_datasets;
	GdalImageReader::RasterInfo tiled_raster_info;
	QSize tiled_raster_size;  ///< Full virtual raster dimensions in pixels.
	QPoint tiled_origin_tile = {0, 0};
	bool has_tiled_origin_tile = false;

	// Background tile loading
	std::vector<std::thread> worker_threads;
	mutable std::atomic<bool> worker_stop{false};
	mutable std::mutex queue_mutex;
	mutable std::condition_variable queue_cv;
	mutable TileQueue tile_queue;
	mutable QHash<quint64, TileQueue::iterator> queued_tiles;  ///< Keys of requests still waiting in the queue.
	mutable QSet<quint64> loading_tiles;  ///< Keys of requests currently being read by workers.

	// Tile cache (mutable: drawTemplate is const but caching is a display concern)
	mutable LruOrder tile_cache_lru;
	mutable QHash<quint64, CachedTileEntry> tile_cache;
	mutable qsizetype tile_cache_bytes = 0;
	qsizetype tile_cache_budget_bytes = 512 * 1024 * 1024;  ///< Roughly 512 MiB of decoded tiles.
	mutable std::atomic<int> active_subsampling{0};
	mutable std::atomic<quint64> active_request_generation{0};
	mutable RequestWindow current_request_window;
	int worker_thread_count = 4;
	int gdal_max_connections = 4;
	mutable int debug_draw_logs_remaining = 0;
	mutable int debug_tile_logs_remaining = 0;

	friend class ::GdalTiledTemplateTest;
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_GDAL_TEMPLATE_H
