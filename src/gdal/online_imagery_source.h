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

#include <QSize>
#include <QString>

namespace OpenOrienteering {


/**
 * Describes an online imagery source detected from user input.
 *
 * Phase 1 supports only tiled sources: XYZ/TMS URL templates and ArcGIS
 * cached MapServer services.
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
	QString normalized_url;
	QSize tile_size;
	int max_tile_level = -1;
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_ONLINE_IMAGERY_SOURCE_H
