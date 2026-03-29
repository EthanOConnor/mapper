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

#include "online_imagery_template_builder.h"

#include <cmath>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QRegularExpression>
#include <QString>
#include <QTextStream>
#include <QUrl>

#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map_coord.h"


namespace OpenOrienteering {

namespace {

/// Full width/height of the EPSG:3857 world in meters.
constexpr double web_mercator_extent = 20037508.342789244;

/// Convert degrees to radians.
double degToRad(double deg) { return deg * M_PI / 180.0; }

/// Derive a URL slug from a display name.
QString slugify(const QString& name)
{
	auto slug = name.toLower();
	slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
	slug.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
	if (slug.length() > 30)
		slug.truncate(30);
	return slug.isEmpty() ? QStringLiteral("online") : slug;
}

/// Compute a short hex hash of a string.
QString shortHash(const QString& input)
{
	auto hash = QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Sha1);
	return QString::fromLatin1(hash.toHex().left(6));
}

/// Extract a display name from a URL.
QString displayNameFromUrl(const QString& url)
{
	QUrl parsed(url);
	auto host = parsed.host();
	// Use subdomain-stripped host as display name.
	auto parts = host.split(QLatin1Char('.'));
	if (parts.size() >= 2)
		return parts[parts.size() - 2];
	return host.isEmpty() ? QStringLiteral("imagery") : host;
}

}  // namespace


// static
OnlineImageryTemplateBuilder::ClassifyResult
OnlineImageryTemplateBuilder::classifyUrl(const QString& input)
{
	ClassifyResult result;
	auto trimmed = input.trimmed();
	result.source.original_input = trimmed;

	if (trimmed.isEmpty())
	{
		result.error = tr("No URL provided.");
		return result;
	}

	// Reject unsupported patterns early with clear messages.
	if (trimmed.contains(QStringLiteral("{s}")) || trimmed.contains(QStringLiteral("${s}")))
	{
		result.error = tr("Subdomain placeholders ({s}) are not supported yet. "
		                  "Try a URL without {s}.");
		return result;
	}
	if (trimmed.contains(QStringLiteral("@2x")) || trimmed.contains(QStringLiteral("@3x")))
	{
		result.error = tr("HiDPI/retina tile URLs (@2x) are not supported yet.");
		return result;
	}

	// Check for XYZ/TMS URL template.
	bool has_z = trimmed.contains(QStringLiteral("{z}")) || trimmed.contains(QStringLiteral("${z}"));
	bool has_x = trimmed.contains(QStringLiteral("{x}")) || trimmed.contains(QStringLiteral("${x}"));
	bool has_y = trimmed.contains(QStringLiteral("{y}")) || trimmed.contains(QStringLiteral("${y}"));

	if (has_z && has_x && has_y)
	{
		// Normalize to GDAL's ${z}/${x}/${y} format.
		auto normalized = trimmed;
		normalized.replace(QStringLiteral("{z}"), QStringLiteral("${z}"));
		normalized.replace(QStringLiteral("{x}"), QStringLiteral("${x}"));
		normalized.replace(QStringLiteral("{y}"), QStringLiteral("${y}"));

		result.source.kind = OnlineImagerySource::Kind::XyzTiles;
		result.source.normalized_url = normalized;
		result.source.display_name = displayNameFromUrl(trimmed);
		result.source.crs_spec = QStringLiteral("EPSG:3857");
		result.source.tile_size = QSize(256, 256);
		result.source.max_tile_level = 19;  // Most XYZ services cap at z19.
		result.source.y_origin_top = true;
		return result;
	}

	// Check for ArcGIS MapServer URL.
	if (trimmed.contains(QStringLiteral("/MapServer"), Qt::CaseInsensitive))
	{
		// Derive service root by trimming /tile/... suffix.
		auto root = trimmed;
		auto tile_idx = root.indexOf(QStringLiteral("/tile/"), 0, Qt::CaseInsensitive);
		if (tile_idx >= 0)
			root.truncate(tile_idx);

		// Ensure it ends with /MapServer.
		if (!root.endsWith(QStringLiteral("/MapServer"), Qt::CaseInsensitive))
		{
			auto ms_idx = root.lastIndexOf(QStringLiteral("/MapServer"), -1, Qt::CaseInsensitive);
			if (ms_idx >= 0)
				root.truncate(ms_idx + 10);  // length of "/MapServer"
		}

		result.source.kind = OnlineImagerySource::Kind::ArcGisTiledMapServer;
		result.source.normalized_url = root;
		result.source.display_name = displayNameFromUrl(root);
		// CRS, tile_size, max_tile_level populated after metadata fetch.
		return result;
	}

	result.error = tr("Couldn't recognize this imagery link. "
	                  "Supported: XYZ tile URLs ({z}/{x}/{y}) and ArcGIS MapServer links.");
	return result;
}


// static
OnlineImageryTemplateBuilder::GenerateResult
OnlineImageryTemplateBuilder::generateXml(
	const OnlineImagerySource& source,
	const QRectF& map_extent,
	const Georeferencing& georef,
	const QString& map_path)
{
	GenerateResult result;

	if (source.kind == OnlineImagerySource::Kind::Unknown)
	{
		result.error = tr("Unknown source type.");
		return result;
	}

	if (map_extent.isEmpty())
	{
		result.error = tr("Map has no objects to determine the coverage area.");
		return result;
	}

	// Convert map extent to EPSG:3857.
	auto mercator_bbox = mapExtentToWebMercator(map_extent, georef);
	if (mercator_bbox.width() <= 0 || mercator_bbox.height() <= 0)
	{
		result.error = tr("Could not convert map extent to Web Mercator coordinates. "
		                  "Map CRS: %1, georef state: %2")
		              .arg(georef.getProjectedCRSSpec())
		              .arg(int(georef.getState()));
		return result;
	}

	// Determine tile size and max level.
	int tile_size = source.tile_size.width();
	if (tile_size <= 0)
		tile_size = 256;
	int max_level = source.max_tile_level;
	if (max_level <= 0)
		max_level = 20;

	// Snap to tile grid.
	auto crop = snapToTileGrid(mercator_bbox, max_level, tile_size);

	// Compute area in km².
	auto width_m = crop.east - crop.west;
	auto height_m = crop.north - crop.south;
	result.area_km2 = (width_m * height_m) / 1e6;

	// Determine the URL for GDAL's TMS service.
	QString server_url;
	if (source.kind == OnlineImagerySource::Kind::XyzTiles)
	{
		server_url = source.normalized_url;
	}
	else if (source.kind == OnlineImagerySource::Kind::ArcGisTiledMapServer)
	{
		server_url = source.normalized_url + QStringLiteral("/tile/${z}/${y}/${x}");
	}

	// Build GDAL XML.
	QString xml;
	QTextStream out(&xml);
	out << QStringLiteral("<GDAL_WMS>\n");
	out << QStringLiteral("  <!-- Mapper online imagery: ") << source.original_input << QStringLiteral(" -->\n");
	out << QStringLiteral("  <Service name=\"TMS\">\n");
	out << QStringLiteral("    <ServerUrl>") << server_url.toHtmlEscaped() << QStringLiteral("</ServerUrl>\n");
	out << QStringLiteral("  </Service>\n");
	out << QStringLiteral("  <DataWindow>\n");
	out << QStringLiteral("    <UpperLeftX>") << QString::number(crop.west, 'f', 6) << QStringLiteral("</UpperLeftX>\n");
	out << QStringLiteral("    <UpperLeftY>") << QString::number(crop.north, 'f', 6) << QStringLiteral("</UpperLeftY>\n");
	out << QStringLiteral("    <LowerRightX>") << QString::number(crop.east, 'f', 6) << QStringLiteral("</LowerRightX>\n");
	out << QStringLiteral("    <LowerRightY>") << QString::number(crop.south, 'f', 6) << QStringLiteral("</LowerRightY>\n");
	out << QStringLiteral("    <SizeX>") << crop.pixel_width << QStringLiteral("</SizeX>\n");
	out << QStringLiteral("    <SizeY>") << crop.pixel_height << QStringLiteral("</SizeY>\n");
	out << QStringLiteral("    <TileLevel>") << crop.tile_level << QStringLiteral("</TileLevel>\n");
	out << QStringLiteral("    <TileX>") << crop.tile_x_min << QStringLiteral("</TileX>\n");
	out << QStringLiteral("    <TileY>") << crop.tile_y_min << QStringLiteral("</TileY>\n");
	out << QStringLiteral("    <YOrigin>top</YOrigin>\n");
	out << QStringLiteral("  </DataWindow>\n");
	out << QStringLiteral("  <Projection>EPSG:3857</Projection>\n");
	out << QStringLiteral("  <BlockSizeX>") << tile_size << QStringLiteral("</BlockSizeX>\n");
	out << QStringLiteral("  <BlockSizeY>") << tile_size << QStringLiteral("</BlockSizeY>\n");
	out << QStringLiteral("  <BandsCount>3</BandsCount>\n");
	out << QStringLiteral("  <ZeroBlockHttpCodes>404</ZeroBlockHttpCodes>\n");
	out << QStringLiteral("  <Cache />\n");
	out << QStringLiteral("</GDAL_WMS>\n");

	// Write file.
	auto output_path = outputFileName(map_path, source);
	QFile file(output_path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		result.error = tr("Could not write imagery file: %1").arg(file.errorString());
		return result;
	}
	file.write(xml.toUtf8());
	file.close();

	result.xml_path = output_path;
	return result;
}


// static
bool OnlineImageryTemplateBuilder::isGeneratedOnlineTemplate(const QString& file_path)
{
	QFile file(file_path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;

	// Check the first few lines for the marker comment.
	auto header = file.read(1024);
	return header.contains("Mapper online imagery:");
}


// static
QString OnlineImageryTemplateBuilder::outputFileName(
	const QString& map_path, const OnlineImagerySource& source)
{
	QFileInfo map_info(map_path);
	auto map_basename = map_info.completeBaseName();
	auto slug = slugify(source.display_name);
	auto hash = shortHash(source.normalized_url);

	auto filename = QStringLiteral("%1_online_%2_%3.xml").arg(map_basename, slug, hash);
	return map_info.absoluteDir().filePath(filename);
}


// static
QPointF OnlineImageryTemplateBuilder::latLonToWebMercator(double lat_deg, double lon_deg)
{
	double x = lon_deg * web_mercator_extent / 180.0;
	double lat_rad = degToRad(lat_deg);
	double y = std::log(std::tan(M_PI / 4.0 + lat_rad / 2.0)) * web_mercator_extent / M_PI;
	return QPointF(x, y);
}


// static
QRectF OnlineImageryTemplateBuilder::mapExtentToWebMercator(
	const QRectF& map_extent, const Georeferencing& georef)
{
	// Convert map extent corners to geographic coordinates, then to Web Mercator.
	// First convert map coords → projected coords → geographic (lat/lon).
	if (georef.getState() != Georeferencing::Geospatial)
	{
		qDebug("mapExtentToWebMercator: georef state is not Geospatial (%d)", int(georef.getState()));
		return {};
	}

	bool ok = false;
	auto tl_proj = georef.toProjectedCoords(MapCoordF(map_extent.topLeft()));
	auto tl = georef.toGeographicCoords(tl_proj, &ok);
	if (!ok) { qWarning("mapExtentToWebMercator: tl conversion failed, proj=(%f,%f)", tl_proj.x(), tl_proj.y()); return {}; }
	auto tr_proj = georef.toProjectedCoords(MapCoordF(map_extent.topRight()));
	auto tr = georef.toGeographicCoords(tr_proj, &ok);
	if (!ok) { qWarning("mapExtentToWebMercator: tr conversion failed"); return {}; }
	auto bl_proj = georef.toProjectedCoords(MapCoordF(map_extent.bottomLeft()));
	auto bl = georef.toGeographicCoords(bl_proj, &ok);
	if (!ok) { qWarning("mapExtentToWebMercator: bl conversion failed"); return {}; }
	auto br_proj = georef.toProjectedCoords(MapCoordF(map_extent.bottomRight()));
	auto br = georef.toGeographicCoords(br_proj, &ok);
	if (!ok) { qWarning("mapExtentToWebMercator: br conversion failed"); return {}; }

	// Compute the bounding box in Web Mercator from all four corners.
	auto m_tl = latLonToWebMercator(tl.latitude(), tl.longitude());
	auto m_tr = latLonToWebMercator(tr.latitude(), tr.longitude());
	auto m_bl = latLonToWebMercator(bl.latitude(), bl.longitude());
	auto m_br = latLonToWebMercator(br.latitude(), br.longitude());

	double min_x = std::min({m_tl.x(), m_tr.x(), m_bl.x(), m_br.x()});
	double max_x = std::max({m_tl.x(), m_tr.x(), m_bl.x(), m_br.x()});
	double min_y = std::min({m_tl.y(), m_tr.y(), m_bl.y(), m_br.y()});
	double max_y = std::max({m_tl.y(), m_tr.y(), m_bl.y(), m_br.y()});

	// Return as QRectF with geographic convention: (left, bottom, width, height)
	// where left=west, bottom=south. QRectF.top()=south, QRectF.bottom()=north.
	// The snapToTileGrid function accounts for this convention.
	return QRectF(min_x, min_y, max_x - min_x, max_y - min_y);
}


// static
OnlineImageryTemplateBuilder::TileGridCrop
OnlineImageryTemplateBuilder::snapToTileGrid(
	const QRectF& mercator_bbox, int max_tile_level, int tile_size)
{
	TileGridCrop crop;
	crop.tile_level = max_tile_level;

	// At zoom level z, the world is divided into 2^z × 2^z tiles.
	// Each tile covers (2 * web_mercator_extent) / 2^z meters.
	double world_size = 2.0 * web_mercator_extent;
	double num_tiles = std::pow(2.0, max_tile_level);
	double tile_span = world_size / num_tiles;

	// Convert mercator bounds to tile indices (top-origin: Y increases downward).
	// Web Mercator origin is at (-web_mercator_extent, web_mercator_extent) for tile (0,0).
	double origin_x = -web_mercator_extent;
	double origin_y = web_mercator_extent;  // top of the world

	// mercator_bbox uses QRectF convention: top() = min_y (south), bottom() = max_y (north).
	// Tile Y origin is at the top of the world (north pole), Y increases downward.
	double merc_south = mercator_bbox.top();     // min_y
	double merc_north = mercator_bbox.bottom();  // max_y
	crop.tile_x_min = int(std::floor((mercator_bbox.left() - origin_x) / tile_span));
	crop.tile_x_max = int(std::ceil((mercator_bbox.right() - origin_x) / tile_span)) - 1;
	crop.tile_y_min = int(std::floor((origin_y - merc_north) / tile_span));
	crop.tile_y_max = int(std::ceil((origin_y - merc_south) / tile_span)) - 1;

	// Clamp to valid range.
	int max_tile_idx = int(num_tiles) - 1;
	crop.tile_x_min = std::max(0, crop.tile_x_min);
	crop.tile_y_min = std::max(0, crop.tile_y_min);
	crop.tile_x_max = std::min(max_tile_idx, crop.tile_x_max);
	crop.tile_y_max = std::min(max_tile_idx, crop.tile_y_max);

	// Pad tile origin to a power-of-two boundary so GDAL's overview
	// bands align correctly. The GDAL WMS/TMS driver discards the
	// sub-tile offset (TileX/TileY remainder bits) when building
	// overview requests, causing spatially shifted content at coarser
	// zoom levels. Aligning the origin to a multiple of 2^align_bits
	// prevents this for overview factors up to 2^align_bits.
	// Extra tiles outside the coverage return HTTP 404, handled by
	// ZeroBlockHttpCodes in the XML.
	constexpr int align_bits = 5;  // align to 32-tile boundary
	constexpr int align_mask = (1 << align_bits) - 1;
	crop.tile_x_min &= ~align_mask;
	crop.tile_y_min &= ~align_mask;
	crop.tile_x_max |= align_mask;
	crop.tile_y_max |= align_mask;
	crop.tile_x_max = std::min(crop.tile_x_max, max_tile_idx);
	crop.tile_y_max = std::min(crop.tile_y_max, max_tile_idx);

	// Compute the snapped projected bounds directly from tile indices.
	crop.west  = origin_x + crop.tile_x_min * tile_span;
	crop.north = origin_y - crop.tile_y_min * tile_span;
	crop.east  = origin_x + (crop.tile_x_max + 1) * tile_span;
	crop.south = origin_y - (crop.tile_y_max + 1) * tile_span;

	crop.pixel_width = (crop.tile_x_max - crop.tile_x_min + 1) * tile_size;
	crop.pixel_height = (crop.tile_y_max - crop.tile_y_min + 1) * tile_size;

	return crop;
}


}  // namespace OpenOrienteering
