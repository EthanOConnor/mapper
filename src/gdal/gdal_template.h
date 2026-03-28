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
#include <mutex>
#include <thread>
#include <vector>

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QQueue>
#include <QSet>
#include <QSize>
#include <QString>

#include "gdal/gdal_image_reader.h"
#include "templates/template.h"
#include "templates/template_image.h"

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
	/**
	 * Background thread function for loading tiles from GDAL.
	 */
	void tileWorkerLoop();

	/**
	 * Queue a tile for background loading. Safe to call from const context
	 * (drawTemplate) because tile loading is a cache operation, not a
	 * semantic state change.
	 */
	void requestTile(int tile_x, int tile_y) const;

	/**
	 * Called on the UI thread when a tile has been loaded by the worker.
	 */
	void onTileLoaded(quint64 key, QImage tile_image);

	/**
	 * Compute a cache key from tile grid coordinates.
	 */
	static quint64 tileKey(int tile_x, int tile_y);

	// Tiled source state (null/empty when using the non-tiled path)
	GDALDatasetH tiled_dataset = nullptr;
	GdalImageReader::RasterInfo tiled_raster_info;
	QSize tiled_raster_size;  ///< Full virtual raster dimensions in pixels.

	// Background tile loading
	mutable std::thread worker_thread;
	mutable std::atomic<bool> worker_stop{false};
	mutable std::mutex queue_mutex;
	mutable std::condition_variable queue_cv;
	mutable QQueue<QPoint> tile_queue;
	mutable QSet<quint64> loading_tiles;  ///< Keys of in-flight tile requests.

	// Tile cache (mutable: drawTemplate is const but caching is a display concern)
	mutable QHash<quint64, QImage> tile_cache;
	int tile_cache_budget = 256;  ///< Maximum number of cached tiles.
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_GDAL_TEMPLATE_H
