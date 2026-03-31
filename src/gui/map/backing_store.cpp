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


#include "backing_store.h"

#include <cmath>
#include <algorithm>

namespace OpenOrienteering {


BackingStore::BackingStore(int tile_size)
    : tile_size(tile_size)
{
	Q_ASSERT(tile_size > 0);
}


void BackingStore::tilesForViewRect(const QRectF& view_rect,
                                    int& col_min, int& col_max,
                                    int& row_min, int& row_max) const
{
	col_min = static_cast<int>(std::floor(view_rect.left() / tile_size));
	col_max = static_cast<int>(std::floor((view_rect.right() - 1) / tile_size));
	row_min = static_cast<int>(std::floor(view_rect.top() / tile_size));
	row_max = static_cast<int>(std::floor((view_rect.bottom() - 1) / tile_size));
	// For zero-width/height rects, ensure max >= min.
	if (col_max < col_min) col_max = col_min;
	if (row_max < row_min) row_max = row_min;
}


QRectF BackingStore::tileViewRect(TileKey key) const
{
	return QRectF(key.col * tile_size, key.row * tile_size,
	              tile_size, tile_size);
}


QRect BackingStore::tileViewportRect(TileKey key, QPointF viewport_origin) const
{
	return QRect(
		qRound(key.col * tile_size + viewport_origin.x()),
		qRound(key.row * tile_size + viewport_origin.y()),
		tile_size, tile_size
	);
}


BackingTile& BackingStore::ensureTile(TileKey key)
{
	auto it = tiles.find(key);
	if (it != tiles.end())
		return it->second;

	auto& tile = tiles[key];
	tile.image = QImage(tile_size, tile_size, QImage::Format_ARGB32_Premultiplied);
	tile.dirty = true;
	return tile;
}


BackingTile* BackingStore::tile(TileKey key)
{
	auto it = tiles.find(key);
	return it != tiles.end() ? &it->second : nullptr;
}


const BackingTile* BackingStore::tile(TileKey key) const
{
	auto it = tiles.find(key);
	return it != tiles.end() ? &it->second : nullptr;
}


void BackingStore::dirtyViewRect(const QRectF& view_rect)
{
	int col_min, col_max, row_min, row_max;
	tilesForViewRect(view_rect, col_min, col_max, row_min, row_max);

	for (auto& [key, tile] : tiles)
	{
		if (key.col >= col_min && key.col <= col_max
		    && key.row >= row_min && key.row <= row_max)
		{
			tile.dirty = true;
		}
	}
}


void BackingStore::dirtyAll()
{
	for (auto& [key, tile] : tiles)
		tile.dirty = true;
}


void BackingStore::clear()
{
	tiles.clear();
}


void BackingStore::evict(const QRectF& retain_rect)
{
	int col_min, col_max, row_min, row_max;
	tilesForViewRect(retain_rect, col_min, col_max, row_min, row_max);

	for (auto it = tiles.begin(); it != tiles.end(); )
	{
		auto& key = it->first;
		if (key.col < col_min || key.col > col_max
		    || key.row < row_min || key.row > row_max)
		{
			it = tiles.erase(it);
		}
		else
		{
			++it;
		}
	}
}


}  // namespace OpenOrienteering
