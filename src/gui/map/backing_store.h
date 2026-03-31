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
 *
 * The grid is anchored in view space: tile (0,0) covers
 * view coordinates [0, tile_size) x [0, tile_size).
 * Negative indices are valid and cover the negative view-space quadrants.
 */
struct TileKey
{
	int col;
	int row;

	bool operator==(TileKey other) const { return col == other.col && row == other.row; }
	bool operator!=(TileKey other) const { return !(*this == other); }
};


/**
 * Hash function for TileKey, allowing use in std::unordered_map.
 */
struct TileKeyHash
{
	std::size_t operator()(TileKey k) const noexcept
	{
		// Combine col and row with a simple hash.
		auto h1 = std::hash<int>{}(k.col);
		auto h2 = std::hash<int>{}(k.row);
		return h1 ^ (h2 * 2654435761u);
	}
};


/**
 * A single backing-store tile.
 *
 * Each tile owns a QImage that covers a fixed region of view space.
 * The tile tracks whether it is dirty (needs redraw).
 */
struct BackingTile
{
	QImage image;
	bool dirty = true;
};


/**
 * A retained tiled backing store for scene rendering.
 *
 * Instead of a single widget-sized cache image, BackingStore keeps a grid
 * of fixed-size QImage tiles anchored in view space. This means:
 *
 * - Panning reuses tiles that remain visible rather than invalidating
 *   the entire cache.
 * - Only tiles that are actually dirty need to be redrawn.
 * - Overscan tiles outside the current viewport can be retained for
 *   smoother scrolling.
 *
 * The tile grid is conceptually infinite. Tiles are created on demand and
 * evicted when they fall outside a configurable retention margin around
 * the viewport.
 *
 * Coordinates:
 * - "View space" is the MapView coordinate system with origin at view center,
 *   measured in pixels. The widget viewport maps to view space by subtracting
 *   (widget_width/2, widget_height/2) plus any transient pan offset.
 * - Each tile covers a tile_size x tile_size square in view space.
 * - TileKey(col, row) covers view rect [col*tile_size, (col+1)*tile_size) x
 *   [row*tile_size, (row+1)*tile_size).
 */
class BackingStore
{
public:
	/**
	 * Constructs a backing store with the given tile size in pixels.
	 * 256 is a reasonable default; it balances per-tile overhead against
	 * granularity of dirty tracking and memory retention.
	 */
	explicit BackingStore(int tile_size = 256);

	/** Returns the tile size in pixels. */
	int tileSize() const { return tile_size; }

	/**
	 * Returns the set of TileKeys that cover the given view-space rectangle.
	 *
	 * Use this to determine which tiles need to be rendered or composited.
	 */
	void tilesForViewRect(const QRectF& view_rect,
	                      int& col_min, int& col_max,
	                      int& row_min, int& row_max) const;

	/**
	 * Returns the view-space rectangle covered by the given tile.
	 */
	QRectF tileViewRect(TileKey key) const;

	/**
	 * Returns the viewport rectangle for a tile, given the current
	 * widget size and pan offset.
	 *
	 * viewport_origin is QPointF(widget_width/2 + pan_offset.x(),
	 *                            widget_height/2 + pan_offset.y()).
	 */
	QRect tileViewportRect(TileKey key, QPointF viewport_origin) const;

	/**
	 * Ensures a tile exists for the given key and returns a reference.
	 * If the tile does not exist yet, creates it as dirty.
	 */
	BackingTile& ensureTile(TileKey key);

	/**
	 * Returns a pointer to the tile if it exists, or nullptr.
	 */
	BackingTile* tile(TileKey key);
	const BackingTile* tile(TileKey key) const;

	/**
	 * Marks all tiles whose view-space coverage intersects the given
	 * view-space rectangle as dirty.
	 */
	void dirtyViewRect(const QRectF& view_rect);

	/**
	 * Marks all retained tiles as dirty.
	 */
	void dirtyAll();

	/**
	 * Discards all tiles, releasing memory.
	 */
	void clear();

	/**
	 * Evicts tiles that do not intersect the given view-space rectangle
	 * (typically the viewport plus some margin).
	 */
	void evict(const QRectF& retain_rect);

	/**
	 * Returns true if the store has no tiles at all.
	 */
	bool isEmpty() const { return tiles.empty(); }

	/**
	 * Returns the number of tiles currently retained.
	 */
	int tileCount() const { return static_cast<int>(tiles.size()); }

private:
	int tile_size;
	std::unordered_map<TileKey, BackingTile, TileKeyHash> tiles;
};


}  // namespace OpenOrienteering

#endif
