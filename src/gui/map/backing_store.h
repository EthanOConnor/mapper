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


#ifndef OPENORIENTEERING_BACKING_STORE_H
#define OPENORIENTEERING_BACKING_STORE_H

#include <memory>
#include <unordered_map>

#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>

namespace OpenOrienteering {


/**
 * Identifies a tile by its column and row in the tile grid.
 */
struct TileKey
{
	int col;
	int row;

	bool operator==(TileKey other) const { return col == other.col && row == other.row; }
	bool operator!=(TileKey other) const { return !(*this == other); }
};


struct TileKeyHash
{
	std::size_t operator()(TileKey k) const noexcept
	{
		auto h1 = std::hash<int>{}(k.col);
		auto h2 = std::hash<int>{}(k.row);
		return h1 ^ (h2 * 2654435761u);
	}
};


struct BackingTile
{
	enum State { Dirty, InFlight, Clean };

	QImage image;
	State state = Dirty;

	bool dirty() const { return state == Dirty; }
	bool clean() const { return state == Clean; }
};


/**
 * A retained tiled backing store for scene rendering.
 *
 * Tiles are anchored in view space via a movable grid offset.
 * When the view center changes (pan completion), the grid offset is
 * adjusted so existing tiles remain valid without re-rendering.
 *
 * When the zoom or rotation changes, current tiles are promoted to
 * a fallback buffer. The fallback tiles can be composited at a
 * different scale as a temporary backdrop while new tiles render.
 */
class BackingStore
{
public:
	explicit BackingStore(int tile_size = 256);

	int tileSize() const { return tile_size; }

	// --- Grid offset ---

	/** The grid offset shifts how tile keys map to view space.
	 *  Tile (col,row) covers view rect [col*ts + offset.x, ...]. */
	QPointF gridOffset() const { return grid_offset; }

	/** Shifts the grid offset. Tiles keep their keys and content. */
	void adjustGridOffset(QPointF delta);

	/** Resets grid offset to zero. Does not invalidate tiles. */
	void resetGridOffset() { grid_offset = {}; }

	// --- Tile coordinate mapping ---

	void tilesForViewRect(const QRectF& view_rect,
	                      int& col_min, int& col_max,
	                      int& row_min, int& row_max) const;

	/** Returns the view-space rect for a tile (includes grid offset). */
	QRectF tileViewRect(TileKey key) const;

	/** Returns the viewport rect for a tile.
	 *  viewport_origin = QPointF(width/2 + pan_offset.x, height/2 + pan_offset.y). */
	QRect tileViewportRect(TileKey key, QPointF viewport_origin) const;

	// --- Tile access ---

	BackingTile& ensureTile(TileKey key);
	BackingTile* tile(TileKey key);
	const BackingTile* tile(TileKey key) const;

	// --- Dirty tracking ---

	void dirtyViewRect(const QRectF& view_rect);
	void dirtyAll();

	// --- Lifecycle ---

	void clear();
	void evict(const QRectF& retain_rect);
	bool isEmpty() const { return tiles.empty(); }
	int tileCount() const { return static_cast<int>(tiles.size()); }

	/** Returns true if all tiles covering the view rect exist and are clean. */
	bool allTilesClean(const QRectF& view_rect) const;

	// --- Zoom fallback ---

	struct FallbackData
	{
		std::unordered_map<TileKey, BackingTile, TileKeyHash> tiles;
		QPointF grid_offset;
	};

	/** Moves current tiles + grid offset into the fallback buffer.
	 *  Existing fallback is discarded. Current store becomes empty
	 *  with grid offset reset to zero. */
	void promoteToFallback();

	bool hasFallback() const { return fallback != nullptr; }
	const FallbackData* fallbackData() const { return fallback.get(); }
	void clearFallback();

private:
	int tile_size;
	QPointF grid_offset;
	std::unordered_map<TileKey, BackingTile, TileKeyHash> tiles;
	std::unique_ptr<FallbackData> fallback;
};


}  // namespace OpenOrienteering

#endif
