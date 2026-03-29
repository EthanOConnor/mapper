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

#ifndef OPENORIENTEERING_ONLINE_IMAGERY_SOURCE_H
#define OPENORIENTEERING_ONLINE_IMAGERY_SOURCE_H

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>

namespace OpenOrienteering {


/**
 * Describes an online imagery source detected from user input.
 *
 * Phase 1 supports only tiled sources (XYZ and ArcGIS cached MapServer).
 * All fields beyond kind/original_input are populated during detection.
 */
struct OnlineImagerySource
{
	enum class Kind {
		XyzTiles,
		ArcGisTiledMapServer,
		Unknown,
	};

	Kind kind = Kind::Unknown;
	QString display_name;
	QString original_input;
	QString normalized_url;    ///< URL template with {z}/{x}/{y} or ${z}/${y}/${x}.
	QString crs_spec;          ///< e.g. "EPSG:3857"
	QSize tile_size;           ///< e.g. (256, 256)
	int max_tile_level = -1;
	bool y_origin_top = true;

	// ArcGIS-specific (populated from ?f=pjson metadata)
	QPointF tile_origin;       ///< Tile grid origin in projected coordinates.
	QRectF full_extent;        ///< fullExtent from service metadata.
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_ONLINE_IMAGERY_SOURCE_H
