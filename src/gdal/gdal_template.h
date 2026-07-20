/*
 *    Copyright 2019-2020 Kai Pastor
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

#include <cstdint>
#include <memory>
#include <vector>

#include <QCache>
#include <QDeadlineTimer>
#include <QHash>
#include <QImage>
#include <QPoint>
#include <QSet>
#include <QSize>
#include <QString>

#include <gdal_dataset.h>

#include "gdal/gdal_image_reader.h"
#include "templates/raster_resource_manager.h"
#include "templates/template.h"
#include "templates/template_image.h"

class QByteArray;
class QWidget;
class QRectF;

namespace OpenOrienteering {

class GdalTiledTest;

class Map;

struct GdalTileKey
{
	int tile_x = 0;
	int tile_y = 0;
	int subsampling = 1;

	bool operator==(const GdalTileKey& other) const
	{
		return tile_x == other.tile_x
		       && tile_y == other.tile_y
		       && subsampling == other.subsampling;
	}
};

inline uint qHash(const GdalTileKey& key, uint seed = 0)
{
	seed = ::qHash(key.tile_x, seed);
	seed = ::qHash(key.tile_y, seed);
	return ::qHash(key.subsampling, seed);
}


/**
 * Support for geospatial raster data.
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

	bool isTiledSource() const;
	QSize getRasterPixelSize() const override;
	
protected:
	bool loadTemplateFileImpl() override;
	bool postLoadSetup(QWidget* dialog_parent, bool& out_center_in_view) override;
	void unloadTemplateFileImpl() override;

	void updateRenderContext(const ViewRenderContext& context) override;
	QRectF getTemplateExtent() const override;
	void collectRasterTiles(const QRectF& map_clip_rect,
	                        double scale,
	                        bool on_screen,
	                        QVector<RasterTemplateTile>& out) const override;
	
	bool applyCornerPassPoints();
	void setupTiledGeoreferencing();
	void updateTiledPosFromGeoreferencing();

private:
	friend class GdalTiledTest;

	struct TileWindow
	{
		int tile_x_min = 0;
		int tile_y_min = 0;
		int tile_x_max = -1;
		int tile_y_max = -1;
		int subsampling = 1;

		bool isEmpty() const
		{
			return tile_x_min > tile_x_max || tile_y_min > tile_y_max;
		}

		qint64 tileCount() const
		{
			return isEmpty() ? 0
			       : qint64(tile_x_max - tile_x_min + 1)
			           * qint64(tile_y_max - tile_y_min + 1);
		}

		bool operator==(const TileWindow& other) const
		{
			return tile_x_min == other.tile_x_min
			       && tile_y_min == other.tile_y_min
			       && tile_x_max == other.tile_x_max
			       && tile_y_max == other.tile_y_max
			       && subsampling == other.subsampling;
		}

		bool intersects(const TileWindow& other) const
		{
			return subsampling == other.subsampling
			       && !isEmpty() && !other.isEmpty()
			       && tile_x_min <= other.tile_x_max
			       && tile_x_max >= other.tile_x_min
			       && tile_y_min <= other.tile_y_max
			       && tile_y_max >= other.tile_y_min;
		}
	};

	void shutdownTiledSource();
	void queueWantedTiles(const TileWindow& window, bool replace_pending_tiles);
	QImage readTileImage(int tile_x, int tile_y, int subsampling) const;
	static QImage readTileImage(
		const std::shared_ptr<GDALDataset>& dataset,
		const GdalImageReader::RasterInfo& raster_info,
		const QSize& raster_size,
		int tile_x,
		int tile_y,
		int subsampling,
		const RasterResourceManager::CancellationToken* cancellation
	);
	void onTileLoaded(const GdalTileKey& key, QImage tile_image);
	void onTileLoadFailed(const GdalTileKey& key);
	void markTileAreaDirty(int tile_x, int tile_y, int subsampling);
	TileWindow tileWindowForMapRect(const QRectF& map_rect, int subsampling) const;
	TileWindow screenTileWindowForMapRect(const QRectF& map_rect, int subsampling) const;
	const QImage* findBestCachedTile(int tile_x, int tile_y, int subsampling, QRectF* source_rect) const;

	static bool readTmsTileOrigin(const QString& template_path, QPoint* origin_tile);
	static GdalTileKey tileKey(int tile_x, int tile_y, int subsampling);
	static int chooseTileSubsampling(double scale, const QSize& block_size);
	static QRect sourceRectForTile(const QSize& raster_size,
	                               const QSize& block_size,
	                               int tile_x,
	                               int tile_y,
	                               int subsampling);
	static QRect decodedSourceRectForTile(const QSize& raster_size,
	                                      const QSize& block_size,
	                                      int tile_x,
	                                      int tile_y,
	                                      int subsampling);
	static QRectF sourceRectWithinCachedTile(const QRect& desired_rect,
	                                         const QRect& cached_rect,
	                                         const QSize& cached_image_size);
	int chooseTiledSubsampling(double scale) const;
	bool isTiledSubsamplingAligned(int subsampling) const;
	qsizetype screenTileAdmissionBudget() const;
	int workerCountForSource() const;

	struct TileFailure
	{
		int attempts = 0;
		QDeadlineTimer retry;
	};

	std::shared_ptr<GDALDataset> tiled_dataset;
	GdalImageReader::RasterInfo tiled_raster_info;
	QSize tiled_raster_size;
	TileWindow wanted_window;
	QPoint tiled_origin_tile;
	bool has_tiled_origin_tile = false;

	RasterResourceManager::Owner raster_owner =
		RasterResourceManager::instance().createOwner();
	QSet<GdalTileKey> queued_tiles;
	QHash<GdalTileKey, TileFailure> failed_tiles;
	TileWindow attempted_window;
	QSet<GdalTileKey> attempted_tiles;

	QCache<GdalTileKey, QImage> tile_cache;
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_GDAL_TEMPLATE_H
