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

#ifndef OPENORIENTEERING_ONLINE_IMAGERY_TEMPLATE_BUILDER_H
#define OPENORIENTEERING_ONLINE_IMAGERY_TEMPLATE_BUILDER_H

#include <QCoreApplication>
#include <QPointF>
#include <QRectF>
#include <QString>

#include "gdal/online_imagery_source.h"

namespace OpenOrienteering {

class Georeferencing;
class GdalTiledTest;


/**
 * Detects, normalizes, and generates GDAL XML files for online imagery sources.
 */
class OnlineImageryTemplateBuilder
{
	Q_DECLARE_TR_FUNCTIONS(OpenOrienteering::OnlineImageryTemplateBuilder)

public:
	struct ClassifyResult
	{
		OnlineImagerySource source;
		QString error;
	};

	struct GenerateResult
	{
		QString xml_path;
		double area_km2 = 0;
		QString error;
	};

	static ClassifyResult classifyUrl(const QString& input);

	static GenerateResult generateXml(
		const OnlineImagerySource& source,
		const QString& template_name,
		const QRectF& map_extent,
		const Georeferencing& georef,
		const QString& map_path);

	static QString outputFileName(
		const QString& map_path,
		const OnlineImagerySource& source,
		const QString& template_name);

	static constexpr double area_warning_threshold_km2 = 200.0;

private:
	friend class GdalTiledTest;

	struct TileGridCrop
	{
		int tile_x_min = 0;
		int tile_y_min = 0;
		int tile_x_max = 0;
		int tile_y_max = 0;
		int tile_level = 0;
		double west = 0;
		double north = 0;
		double east = 0;
		double south = 0;
		int pixel_width = 0;
		int pixel_height = 0;
	};

	static QPointF latLonToWebMercator(double lat_deg, double lon_deg);
	static QRectF mapExtentToWebMercator(const QRectF& map_extent, const Georeferencing& georef);
	static TileGridCrop snapToTileGrid(const QRectF& mercator_bbox, int max_tile_level, int tile_size);
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_ONLINE_IMAGERY_TEMPLATE_BUILDER_H
