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
#include <QTextStream>
#include <QUrl>

#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map_coord.h"

namespace OpenOrienteering {

namespace {

constexpr double web_mercator_extent = 20037508.342789244;

double degToRad(double deg)
{
	return deg * M_PI / 180.0;
}

QString slugify(const QString& name)
{
	auto slug = name.toLower();
	slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
	slug.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
	if (slug.length() > 30)
		slug.truncate(30);
	return slug.isEmpty() ? QStringLiteral("online") : slug;
}

QString shortHash(const QString& input)
{
	auto hash = QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Sha1);
	return QString::fromLatin1(hash.toHex().left(6));
}

QString displayNameFromUrl(const QString& url)
{
	QUrl parsed(url);
	auto host = parsed.host();
	auto parts = host.split(QLatin1Char('.'));
	if (parts.size() >= 2)
		return parts[parts.size() - 2];
	return host.isEmpty() ? QStringLiteral("imagery") : host;
}

QString effectiveTemplateName(const OnlineImagerySource& source, const QString& template_name)
{
	auto const trimmed_name = template_name.trimmed();
	return trimmed_name.isEmpty() ? source.display_name : trimmed_name;
}

}  // namespace


OnlineImageryTemplateBuilder::ClassifyResult
OnlineImageryTemplateBuilder::classifyUrl(const QString& input)
{
	ClassifyResult result;
	auto trimmed = input.trimmed();

	if (trimmed.isEmpty())
	{
		result.error = tr("No URL provided.");
		return result;
	}

	if (trimmed.contains(QStringLiteral("{s}")) || trimmed.contains(QStringLiteral("${s}")))
	{
		result.error = tr("Subdomain placeholders ({s}) are not supported yet. Try a URL without {s}.");
		return result;
	}
	if (trimmed.contains(QStringLiteral("@2x")) || trimmed.contains(QStringLiteral("@3x")))
	{
		result.error = tr("HiDPI/retina tile URLs (@2x) are not supported yet.");
		return result;
	}

	if (trimmed.contains(QStringLiteral("/MapServer"), Qt::CaseInsensitive))
	{
		auto root = trimmed;
		auto tile_index = root.indexOf(QStringLiteral("/tile/"), 0, Qt::CaseInsensitive);
		if (tile_index >= 0)
			root.truncate(tile_index);

		if (!root.endsWith(QStringLiteral("/MapServer"), Qt::CaseInsensitive))
		{
			auto ms_index = root.lastIndexOf(QStringLiteral("/MapServer"), -1, Qt::CaseInsensitive);
			if (ms_index >= 0)
				root.truncate(ms_index + 10);
		}

		result.source.kind = OnlineImagerySource::Kind::ArcGisTiledMapServer;
		result.source.normalized_url = root;
		result.source.display_name = displayNameFromUrl(root);
		result.source.tile_size = QSize(256, 256);
		result.source.max_tile_level = 20;
		return result;
	}

	auto const has_z = trimmed.contains(QStringLiteral("{z}")) || trimmed.contains(QStringLiteral("${z}"));
	auto const has_x = trimmed.contains(QStringLiteral("{x}")) || trimmed.contains(QStringLiteral("${x}"));
	auto const has_y = trimmed.contains(QStringLiteral("{y}")) || trimmed.contains(QStringLiteral("${y}"));

	if (has_z && has_x && has_y)
	{
		auto normalized = trimmed;
		normalized.replace(QStringLiteral("{z}"), QStringLiteral("${z}"));
		normalized.replace(QStringLiteral("{x}"), QStringLiteral("${x}"));
		normalized.replace(QStringLiteral("{y}"), QStringLiteral("${y}"));

		result.source.kind = OnlineImagerySource::Kind::XyzTiles;
		result.source.normalized_url = normalized;
		result.source.display_name = displayNameFromUrl(trimmed);
		result.source.tile_size = QSize(256, 256);
		result.source.max_tile_level = 19;
		return result;
	}

	result.error = tr("Couldn't recognize this imagery link. Supported: XYZ tile URLs ({z}/{x}/{y}) and ArcGIS MapServer links.");
	return result;
}


OnlineImageryTemplateBuilder::GenerateResult
OnlineImageryTemplateBuilder::generateXml(
	const OnlineImagerySource& source,
	const QString& template_name,
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
		result.error = tr("No map coverage area was selected.");
		return result;
	}

	auto mercator_bbox = mapExtentToWebMercator(map_extent, georef);
	if (mercator_bbox.width() <= 0 || mercator_bbox.height() <= 0)
	{
		result.error = tr("Could not convert the selected coverage area to Web Mercator coordinates.");
		return result;
	}

	auto tile_size = source.tile_size.width();
	if (tile_size <= 0)
		tile_size = 256;
	auto max_level = source.max_tile_level;
	if (max_level <= 0)
		max_level = 20;

	auto crop = snapToTileGrid(mercator_bbox, max_level, tile_size);
	auto width_m = crop.east - crop.west;
	auto height_m = crop.north - crop.south;
	result.area_km2 = (width_m * height_m) / 1e6;

	QString server_url;
	switch (source.kind)
	{
	case OnlineImagerySource::Kind::XyzTiles:
		server_url = source.normalized_url;
		break;
	case OnlineImagerySource::Kind::ArcGisTiledMapServer:
		server_url = source.normalized_url + QStringLiteral("/tile/${z}/${y}/${x}");
		break;
	case OnlineImagerySource::Kind::Unknown:
		Q_UNREACHABLE();
	}

	QString xml;
	QTextStream out(&xml);
	out << QStringLiteral("<GDAL_WMS>\n");
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
	// Keep GDAL's default per-user cache location instead of baking a
	// machine-local absolute cache path into the generated template file.
	out << QStringLiteral("  <Cache />\n");
	out << QStringLiteral("</GDAL_WMS>\n");

	auto output_path = outputFileName(map_path, source, template_name);
	QFile file(output_path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		result.error = tr("Could not write imagery file: %1").arg(file.errorString());
		return result;
	}
	if (file.write(xml.toUtf8()) < 0)
	{
		result.error = tr("Could not write imagery file: %1").arg(file.errorString());
		return result;
	}

	result.xml_path = output_path;
	return result;
}


QString OnlineImageryTemplateBuilder::outputFileName(
	const QString& map_path,
	const OnlineImagerySource& source,
	const QString& template_name)
{
	QFileInfo map_info(map_path);
	auto map_basename = map_info.completeBaseName();
	auto slug = slugify(effectiveTemplateName(source, template_name));
	auto hash = shortHash(source.normalized_url);
	auto filename = QStringLiteral("%1_%2_online_%3.xml").arg(slug, map_basename, hash);
	return map_info.absoluteDir().filePath(filename);
}


QPointF OnlineImageryTemplateBuilder::latLonToWebMercator(double lat_deg, double lon_deg)
{
	auto x = lon_deg * web_mercator_extent / 180.0;
	auto lat_rad = degToRad(lat_deg);
	auto y = std::log(std::tan(M_PI / 4.0 + lat_rad / 2.0)) * web_mercator_extent / M_PI;
	return QPointF(x, y);
}


QRectF OnlineImageryTemplateBuilder::mapExtentToWebMercator(
	const QRectF& map_extent,
	const Georeferencing& georef)
{
	if (georef.getState() != Georeferencing::Geospatial)
		return {};

	bool ok = false;
	auto tl = georef.toGeographicCoords(georef.toProjectedCoords(MapCoordF(map_extent.topLeft())), &ok);
	if (!ok)
		return {};
	auto tr = georef.toGeographicCoords(georef.toProjectedCoords(MapCoordF(map_extent.topRight())), &ok);
	if (!ok)
		return {};
	auto bl = georef.toGeographicCoords(georef.toProjectedCoords(MapCoordF(map_extent.bottomLeft())), &ok);
	if (!ok)
		return {};
	auto br = georef.toGeographicCoords(georef.toProjectedCoords(MapCoordF(map_extent.bottomRight())), &ok);
	if (!ok)
		return {};

	auto m_tl = latLonToWebMercator(tl.latitude(), tl.longitude());
	auto m_tr = latLonToWebMercator(tr.latitude(), tr.longitude());
	auto m_bl = latLonToWebMercator(bl.latitude(), bl.longitude());
	auto m_br = latLonToWebMercator(br.latitude(), br.longitude());

	auto min_x = std::min({m_tl.x(), m_tr.x(), m_bl.x(), m_br.x()});
	auto max_x = std::max({m_tl.x(), m_tr.x(), m_bl.x(), m_br.x()});
	auto min_y = std::min({m_tl.y(), m_tr.y(), m_bl.y(), m_br.y()});
	auto max_y = std::max({m_tl.y(), m_tr.y(), m_bl.y(), m_br.y()});
	return QRectF(min_x, min_y, max_x - min_x, max_y - min_y);
}


OnlineImageryTemplateBuilder::TileGridCrop
OnlineImageryTemplateBuilder::snapToTileGrid(
	const QRectF& mercator_bbox,
	int max_tile_level,
	int tile_size)
{
	TileGridCrop crop;
	crop.tile_level = max_tile_level;

	auto world_size = 2.0 * web_mercator_extent;
	auto num_tiles = std::pow(2.0, max_tile_level);
	auto tile_span = world_size / num_tiles;
	auto origin_x = -web_mercator_extent;
	auto origin_y = web_mercator_extent;

	auto merc_south = mercator_bbox.top();
	auto merc_north = mercator_bbox.bottom();
	crop.tile_x_min = int(std::floor((mercator_bbox.left() - origin_x) / tile_span));
	crop.tile_x_max = int(std::ceil((mercator_bbox.right() - origin_x) / tile_span)) - 1;
	crop.tile_y_min = int(std::floor((origin_y - merc_north) / tile_span));
	crop.tile_y_max = int(std::ceil((origin_y - merc_south) / tile_span)) - 1;

	auto max_tile_idx = int(num_tiles) - 1;
	crop.tile_x_min = std::max(0, crop.tile_x_min);
	crop.tile_y_min = std::max(0, crop.tile_y_min);
	crop.tile_x_max = std::min(max_tile_idx, crop.tile_x_max);
	crop.tile_y_max = std::min(max_tile_idx, crop.tile_y_max);

	// Pad the cropped origin to a power-of-two tile boundary so GDAL's
	// WMS/TMS overview requests keep the source registration aligned.
	// Extra padded tiles outside the intended coverage are harmless and
	// typically return 404s, which the generated XML already treats as
	// zero blocks.
	constexpr int align_bits = 5;  // 32-tile boundary
	constexpr int align_mask = (1 << align_bits) - 1;
	crop.tile_x_min &= ~align_mask;
	crop.tile_y_min &= ~align_mask;
	crop.tile_x_max |= align_mask;
	crop.tile_y_max |= align_mask;
	crop.tile_x_max = std::min(crop.tile_x_max, max_tile_idx);
	crop.tile_y_max = std::min(crop.tile_y_max, max_tile_idx);

	crop.west = origin_x + crop.tile_x_min * tile_span;
	crop.north = origin_y - crop.tile_y_min * tile_span;
	crop.east = origin_x + (crop.tile_x_max + 1) * tile_span;
	crop.south = origin_y - (crop.tile_y_max + 1) * tile_span;
	crop.pixel_width = (crop.tile_x_max - crop.tile_x_min + 1) * tile_size;
	crop.pixel_height = (crop.tile_y_max - crop.tile_y_min + 1) * tile_size;
	return crop;
}


}  // namespace OpenOrienteering
