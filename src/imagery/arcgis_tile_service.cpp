/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "imagery/arcgis_tile_service.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QUrlQuery>

#include "imagery/manual_imagery_source.h"

namespace OpenOrienteering::imagery {

namespace {

constexpr auto maximum_published_tile_dimension = 4096;
constexpr qint64 maximum_published_tile_pixels =
	qint64(8) * 1024 * 1024;

ArcGisTileServiceResult invalid(QString detail)
{
	ArcGisTileServiceResult result;
	result.outcome = ArcGisTileServiceOutcome::Invalid;
	result.detail = std::move(detail);
	return result;
}

ArcGisTileServiceResult unsupported(QString detail)
{
	ArcGisTileServiceResult result;
	result.outcome = ArcGisTileServiceOutcome::Unsupported;
	result.detail = std::move(detail);
	return result;
}

bool finiteNumber(const QJsonValue& value)
{
	return value.isDouble() && std::isfinite(value.toDouble());
}

bool exactInteger(
	const QJsonValue& value,
	qint64 minimum,
	qint64 maximum,
	qint64* output)
{
	if (!finiteNumber(value))
		return false;
	auto const number = value.toDouble();
	if (std::floor(number) != number
	    || number < double(minimum) || number > double(maximum)
	    || std::abs(number) > 9007199254740991.0)
	{
		return false;
	}
	*output = qint64(number);
	return true;
}

bool finiteMember(
	const QJsonObject& object,
	const QString& name,
	double* output)
{
	auto const value = object.value(name);
	if (!finiteNumber(value))
		return false;
	*output = value.toDouble();
	return true;
}

std::optional<int> normalizedEpsg(
	const QJsonObject& spatial_reference,
	bool* malformed)
{
	*malformed = false;
	for (auto const& name : {
		QStringLiteral("latestWkid"), QStringLiteral("wkid")
	})
	{
		if (!spatial_reference.contains(name))
			continue;
		qint64 wkid = 0;
		if (!exactInteger(
			    spatial_reference.value(name), 1, 999999999, &wkid))
		{
			*malformed = true;
			return std::nullopt;
		}
		if (wkid == 102100 || wkid == 102113 || wkid == 900913)
			return 3857;
		if (wkid <= 99999)
			return int(wkid);
	}
	return std::nullopt;
}

bool validServiceUrl(const QUrl& url)
{
	auto const scheme = url.scheme().toLower();
	return url.isValid() && !url.isRelative() && !url.host().isEmpty()
	       && (scheme == QLatin1String("http")
	           || scheme == QLatin1String("https"))
	       && url.userName().isEmpty() && url.password().isEmpty()
	       && !url.hasFragment()
	       && url.toString(QUrl::FullyEncoded).size() <= 8192;
}

QString filteredServiceQuery(const QUrl& url)
{
	QStringList retained;
	for (auto const& item :
	     url.query(QUrl::FullyEncoded).split(
		     QLatin1Char('&'), Qt::SkipEmptyParts))
	{
		auto const separator = item.indexOf(QLatin1Char('='));
		auto const encoded_name =
			separator < 0 ? item : item.left(separator);
		auto const name =
			QUrl::fromPercentEncoding(encoded_name.toUtf8());
		if (name.compare(
			    QStringLiteral("f"), Qt::CaseInsensitive) != 0
		    && name.compare(
			       QStringLiteral("callback"), Qt::CaseInsensitive) != 0)
		{
			retained.push_back(item);
		}
	}
	return retained.join(QLatin1Char('&'));
}

std::optional<QUrl> normalizedServiceUrl(const QUrl& input)
{
	if (!validServiceUrl(input))
		return std::nullopt;
	static const QRegularExpression pattern(
		QStringLiteral(
			"^(.*/rest/services/.*/(?:MapServer|ImageServer))"
			"(?:/tile/[^/]+/[^/]+/[^/]+)?/?$"
		),
		QRegularExpression::CaseInsensitiveOption
	);
	auto const match = pattern.match(input.path());
	if (!match.hasMatch())
		return std::nullopt;

	auto result = input;
	result.setPath(match.captured(1));
	result.setFragment({});
	result.setQuery(filteredServiceQuery(input), QUrl::StrictMode);
	return result;
}

double snapNearInteger(double value)
{
	auto const nearest = std::round(value);
	auto const tolerance =
		std::max(1.0, std::abs(value)) * 1.0e-10;
	return std::abs(value - nearest) <= tolerance
		? nearest
		: value;
}

bool extentIndex(
	double value,
	bool upper,
	qint64 minimum,
	qint64 maximum,
	qint64* output)
{
	if (!std::isfinite(value))
		return false;
	auto const snapped = snapNearInteger(value);
	auto const indexed = upper
		? std::ceil(snapped) - 1.0
		: std::floor(snapped);
	if (!std::isfinite(indexed)
	    || indexed < double(minimum) || indexed > double(maximum))
	{
		return false;
	}
	*output = qint64(indexed);
	return true;
}

bool coveringDimension(double value, qint64* output)
{
	if (!std::isfinite(value) || value <= 0)
		return false;
	auto const dimension = std::ceil(snapNearInteger(value));
	if (!std::isfinite(dimension) || dimension < 1
	    || dimension > double(std::numeric_limits<qint64>::max()))
	{
		return false;
	}
	*output = qint64(dimension);
	return true;
}

QString generatedId(const QUrl& service_url)
{
	auto const digest = QCryptographicHash::hash(
		service_url.toString(QUrl::FullyEncoded).toUtf8(),
		QCryptographicHash::Sha256
	).toHex();
	return QStringLiteral("arcgis-%1")
		.arg(QString::fromLatin1(digest.first(16)));
}

QString serviceTitle(
	const QJsonObject& root,
	const QUrl& service_url)
{
	for (auto const& value : {
		root.value(QStringLiteral("name")),
		root.value(QStringLiteral("mapName")),
	})
	{
		if (value.isString() && !value.toString().trimmed().isEmpty())
			return value.toString().trimmed();
	}
	auto const document_info =
		root.value(QStringLiteral("documentInfo")).toObject();
	auto const title =
		document_info.value(QStringLiteral("Title")).toString().trimmed();
	if (!title.isEmpty())
		return title;

	auto path = service_url.path();
	static const QRegularExpression suffix(
		QStringLiteral("/(?:MapServer|ImageServer)$"),
		QRegularExpression::CaseInsensitiveOption
	);
	path.remove(suffix);
	auto result = path.section(QLatin1Char('/'), -1);
	if (result.isEmpty())
		result = service_url.host().toLower();
	return result;
}

QString mediaType(const QJsonObject& tile_info)
{
	if (!tile_info.value(QStringLiteral("format")).isString())
		return {};
	auto format =
		tile_info.value(QStringLiteral("format"))
			.toString().trimmed().toUpper();
	if (format == QLatin1String("JPG")
	    || format == QLatin1String("JPEG"))
	{
		return QStringLiteral("image/jpeg");
	}
	if (format.startsWith(QLatin1String("PNG"))
	    || format == QLatin1String("MIXED"))
	{
		return QStringLiteral("image/png");
	}
	return {};
}

QString tileTemplate(const QUrl& service_url)
{
	auto endpoint = service_url;
	auto const query = QUrlQuery(endpoint);
	endpoint.setQuery(QString {});
	auto base = endpoint.toString(QUrl::FullyEncoded);
	while (base.endsWith(QLatin1Char('/')))
		base.chop(1);
	QString result =
		base + QStringLiteral("/tile/{z}/{y}/{x}");
	auto const encoded_query =
		query.toString(QUrl::FullyEncoded);
	if (!encoded_query.isEmpty())
	{
		result += QLatin1Char('?');
		result += encoded_query;
	}
	return result;
}

}  // namespace

bool ArcGisTileServiceResult::resolved() const noexcept
{
	return outcome == ArcGisTileServiceOutcome::Resolved
	       && source.has_value();
}

ArcGisTileServiceResult ArcGisTileService::parse(
	const QByteArray& pjson,
	const QUrl& service_url,
	const ArcGisTileServiceSettings& settings)
{
	if (pjson.isEmpty())
		return invalid(tr("ArcGIS metadata is empty."));
	if (pjson.size() > maximum_metadata_size)
	{
		return invalid(tr(
			"ArcGIS metadata exceeds the 1 MiB safety limit."
		));
	}
	auto const normalized_url = normalizedServiceUrl(service_url);
	if (!normalized_url)
	{
		return invalid(tr(
			"The ArcGIS service URL must identify an HTTP(S) MapServer or ImageServer endpoint."
		));
	}

	QJsonParseError parse_error;
	auto const document =
		QJsonDocument::fromJson(pjson, &parse_error);
	if (parse_error.error != QJsonParseError::NoError
	    || !document.isObject())
	{
		return invalid(tr(
			"ArcGIS metadata is not a valid JSON object."
		));
	}
	auto const root = document.object();
	if (root.value(QStringLiteral("error")).isObject())
	{
		auto const error_object =
			root.value(QStringLiteral("error")).toObject();
		qint64 code = 0;
		if (exactInteger(
			    error_object.value(QStringLiteral("code")),
			    0, std::numeric_limits<int>::max(), &code))
		{
			return invalid(tr(
				"ArcGIS service returned error code %1."
			).arg(code));
		}
		return invalid(tr(
			"ArcGIS service returned an error response."
		));
	}
	if (root.contains(QStringLiteral("singleFusedMapCache"))
	    && !root.value(QStringLiteral("singleFusedMapCache")).isBool())
	{
		return invalid(tr(
			"ArcGIS singleFusedMapCache must be a boolean."
		));
	}
	if (root.value(QStringLiteral("singleFusedMapCache")).isBool()
	    && !root.value(QStringLiteral("singleFusedMapCache")).toBool())
	{
		return unsupported(tr(
			"The ArcGIS service is not a fused tile cache."
		));
	}
	if (!root.value(QStringLiteral("tileInfo")).isObject())
	{
		return unsupported(tr(
			"The ArcGIS service does not publish cached tile metadata."
		));
	}
	auto const tile_info =
		root.value(QStringLiteral("tileInfo")).toObject();

	qint64 rows = 0;
	qint64 columns = 0;
	if (!exactInteger(
		    tile_info.value(QStringLiteral("rows")),
		    1, maximum_published_tile_dimension, &rows)
	    || !exactInteger(
		    tile_info.value(QStringLiteral("cols")),
		    1, maximum_published_tile_dimension, &columns))
	{
		return invalid(tr(
			"ArcGIS tile dimensions are missing or invalid."
		));
	}
	if (rows * columns > maximum_published_tile_pixels)
	{
		return unsupported(tr(
			"ArcGIS tiles exceed the bounded raster decode profile."
		));
	}
	if (!runtimeSupportsTileSize(
		    QSize(int(columns), int(rows))))
	{
		return unsupported(tr(
			"ArcGIS tiles exceed this build's raster execution profile."
		));
	}

	if (!tile_info.value(QStringLiteral("origin")).isObject())
	{
		return invalid(tr(
			"ArcGIS tile origin is missing."
		));
	}
	auto const origin =
		tile_info.value(QStringLiteral("origin")).toObject();
	double origin_x = 0;
	double origin_y = 0;
	if (!finiteMember(origin, QStringLiteral("x"), &origin_x)
	    || !finiteMember(origin, QStringLiteral("y"), &origin_y))
	{
		return invalid(tr(
			"ArcGIS tile origin must contain finite x and y coordinates."
		));
	}

	if (!tile_info.value(QStringLiteral("spatialReference")).isObject())
	{
		return unsupported(tr(
			"ArcGIS tile metadata has no numeric spatial reference."
		));
	}
	bool malformed_crs = false;
	auto const epsg = normalizedEpsg(
		tile_info.value(QStringLiteral("spatialReference")).toObject(),
		&malformed_crs
	);
	if (malformed_crs)
	{
		return invalid(tr(
			"ArcGIS spatial reference identifiers are malformed."
		));
	}
	if (!epsg)
	{
		return unsupported(tr(
			"ArcGIS tile metadata uses a spatial reference that cannot be normalized to EPSG."
		));
	}
	if (root.contains(QStringLiteral("spatialReference")))
	{
		if (!root.value(QStringLiteral("spatialReference")).isObject())
		{
			return invalid(tr(
				"ArcGIS service spatialReference is malformed."
			));
		}
		bool malformed_root_crs = false;
		auto const root_epsg = normalizedEpsg(
			root.value(QStringLiteral("spatialReference")).toObject(),
			&malformed_root_crs
		);
		if (malformed_root_crs)
		{
			return invalid(tr(
				"ArcGIS service spatial reference is malformed."
			));
		}
		if (!root_epsg || *root_epsg != *epsg)
		{
			return unsupported(tr(
				"ArcGIS service and tileInfo use different spatial references."
			));
		}
	}
	auto const image_service =
		normalized_url->path().endsWith(
			QStringLiteral("/ImageServer"), Qt::CaseInsensitive
		);
	if (image_service && root.contains(QStringLiteral("cacheType")))
	{
		if (!root.value(QStringLiteral("cacheType")).isString())
		{
			return invalid(tr(
				"ArcGIS ImageServer cacheType must be a string."
			));
		}
		if (root.value(QStringLiteral("cacheType")).toString()
		      .compare(QStringLiteral("Map"), Qt::CaseInsensitive) != 0)
		{
			return unsupported(tr(
				"ArcGIS ImageServer elevation and raster caches are not supported."
			));
		}
	}

	if (!root.value(QStringLiteral("fullExtent")).isObject())
	{
		return unsupported(tr(
			"ArcGIS fullExtent is required to derive finite tile matrix dimensions."
		));
	}
	auto const full_extent =
		root.value(QStringLiteral("fullExtent")).toObject();
	double west = 0;
	double south = 0;
	double east = 0;
	double north = 0;
	if (!finiteMember(full_extent, QStringLiteral("xmin"), &west)
	    || !finiteMember(full_extent, QStringLiteral("ymin"), &south)
	    || !finiteMember(full_extent, QStringLiteral("xmax"), &east)
	    || !finiteMember(full_extent, QStringLiteral("ymax"), &north)
	    || !(west < east) || !(south < north))
	{
		return invalid(tr(
			"ArcGIS fullExtent must contain ordered finite bounds."
		));
	}
	if (full_extent.contains(QStringLiteral("spatialReference")))
	{
		if (!full_extent.value(
			    QStringLiteral("spatialReference")).isObject())
		{
			return invalid(tr(
				"ArcGIS fullExtent spatialReference is malformed."
			));
		}
		bool malformed_extent_crs = false;
		auto const extent_epsg = normalizedEpsg(
			full_extent.value(
				QStringLiteral("spatialReference")).toObject(),
			&malformed_extent_crs
		);
		if (malformed_extent_crs)
		{
			return invalid(tr(
				"ArcGIS fullExtent spatial reference is malformed."
			));
		}
		if (!extent_epsg || *extent_epsg != *epsg)
		{
			return unsupported(tr(
				"ArcGIS fullExtent and tileInfo use different spatial references."
			));
		}
	}
	if (!tile_info.value(QStringLiteral("lods")).isArray())
	{
		return invalid(tr(
			"ArcGIS tile metadata has no LOD array."
		));
	}
	auto const lods =
		tile_info.value(QStringLiteral("lods")).toArray();
	if (lods.isEmpty() || lods.size() > maximum_lods)
	{
		return unsupported(tr(
			"ArcGIS LOD count is outside the supported range."
		));
	}

	struct PublishedLod
	{
		int level = -1;
		double resolution = 0;
	};
	QVector<PublishedLod> published_lods;
	published_lods.reserve(lods.size());
	for (qsizetype index = 0; index < lods.size(); ++index)
	{
		if (!lods.at(index).isObject())
		{
			return invalid(tr(
				"ArcGIS LOD entries must be objects."
			));
		}
		auto const lod = lods.at(index).toObject();
		qint64 level = -1;
		double resolution = 0;
		if (!exactInteger(
			    lod.value(QStringLiteral("level")),
			    0, maximum_lods - 1, &level)
		    || !finiteMember(
			    lod, QStringLiteral("resolution"), &resolution)
		    || !(resolution > 0))
		{
			return invalid(tr(
				"ArcGIS LOD level or resolution is invalid."
			));
		}
		if (lod.contains(QStringLiteral("scale")))
		{
			double scale = 0;
			if (!finiteMember(lod, QStringLiteral("scale"), &scale)
			    || !(scale > 0))
			{
				return invalid(tr(
					"ArcGIS LOD scales must be finite and positive."
				));
			}
		}
		published_lods.push_back({ int(level), resolution });
	}
	std::sort(
		published_lods.begin(), published_lods.end(),
		[](auto const& first, auto const& second) {
			return first.level < second.level;
		}
	);
	for (qsizetype index = 0;
	     index < published_lods.size(); ++index)
	{
		if (index > 0
		    && published_lods.at(index - 1).level
		         == published_lods.at(index).level)
		{
			return invalid(tr(
				"ArcGIS LOD levels must be unique."
			));
		}
		if (published_lods.at(index).level != index)
		{
			return unsupported(tr(
				"ArcGIS LOD levels must begin at zero and be contiguous."
			));
		}
	}

	auto const base_resolution = published_lods.first().resolution;
	for (qsizetype index = 0;
	     index < published_lods.size(); ++index)
	{
		auto const expected =
			base_resolution / std::ldexp(1.0, int(index));
		auto const published =
			published_lods.at(index).resolution;
		auto const tolerance =
			std::max({ 1.0e-15, expected, published }) * 1.0e-8;
		if (std::abs(published - expected) > tolerance)
		{
			return unsupported(tr(
				"ArcGIS LOD resolutions do not form a dyadic pyramid."
			));
		}
	}

	auto const highest_zoom = int(published_lods.size()) - 1;
	auto minimum_zoom = 0;
	auto maximum_zoom = highest_zoom;
	for (auto const& item : {
		std::pair { QStringLiteral("minLOD"), &minimum_zoom },
		std::pair { QStringLiteral("maxLOD"), &maximum_zoom },
	})
	{
		if (!root.contains(item.first))
			continue;
		qint64 value = 0;
		if (!exactInteger(
			    root.value(item.first), 0, highest_zoom, &value))
		{
			return invalid(tr(
				"ArcGIS %1 is outside the published LOD range."
			).arg(item.first));
		}
		*item.second = int(value);
	}
	if (minimum_zoom > maximum_zoom)
	{
		return invalid(tr(
			"ArcGIS minLOD follows maxLOD."
		));
	}

	auto const base_tile_width =
		base_resolution * double(columns);
	auto const base_tile_height =
		base_resolution * double(rows);
	qint64 base_matrix_width = 0;
	qint64 base_matrix_height = 0;
	constexpr auto web_mercator_half_world = 20037508.342789244;
	auto const canonical_web_mercator =
		*epsg == 3857 && rows == columns
		&& std::abs(origin_x + web_mercator_half_world) <= 0.02
		&& std::abs(origin_y - web_mercator_half_world) <= 0.02
		&& std::abs(
			base_resolution
			- (2 * web_mercator_half_world) / double(columns)
		) <= base_resolution * 1.0e-8;
	auto runtime_base_resolution = base_resolution;
	if (canonical_web_mercator)
	{
		origin_x = -web_mercator_half_world;
		origin_y = web_mercator_half_world;
		runtime_base_resolution =
			(2 * web_mercator_half_world) / double(columns);
		west = std::max(west, -web_mercator_half_world);
		south = std::max(south, -web_mercator_half_world);
		east = std::min(east, web_mercator_half_world);
		north = std::min(north, web_mercator_half_world);
		if (!(west < east) || !(south < north))
		{
			return unsupported(tr(
				"ArcGIS fullExtent does not intersect WebMercatorQuad."
			));
		}
		base_matrix_width = 1;
		base_matrix_height = 1;
	}
	else
	{
		auto const extent_tolerance =
			std::max({
				1.0, std::abs(origin_x), std::abs(origin_y),
				std::abs(west), std::abs(east),
				std::abs(south), std::abs(north),
			}) * 1.0e-12;
		if (origin_x > west + extent_tolerance
		    || origin_y < north - extent_tolerance)
		{
			return unsupported(tr(
				"ArcGIS fullExtent lies outside the top-left tile origin."
			));
		}
		if (!coveringDimension(
			    (east - origin_x) / base_tile_width,
			    &base_matrix_width)
		    || !coveringDimension(
			    (origin_y - south) / base_tile_height,
			    &base_matrix_height))
		{
			return unsupported(tr(
				"ArcGIS fullExtent cannot produce finite tile matrix dimensions."
			));
		}
	}
	auto const factor = qint64(1) << highest_zoom;
	if (base_matrix_width
	      > std::numeric_limits<qint64>::max() / factor
	    || base_matrix_height
	      > std::numeric_limits<qint64>::max() / factor)
	{
		return unsupported(tr(
			"ArcGIS tile matrix dimensions overflow the runtime model."
		));
	}

	TileMatrixSet matrix_set;
	matrix_set.id = canonical_web_mercator && columns == 256
		? QStringLiteral("WebMercatorQuad")
		: canonical_web_mercator && columns == 512
		  ? QStringLiteral("WebMercatorQuad512")
		  : QStringLiteral("ArcGISCacheEPSG%1").arg(*epsg);
	matrix_set.crs =
		QStringLiteral("EPSG:%1").arg(*epsg);
	matrix_set.matrices.reserve(lods.size());
	QVector<TileMatrixLimits> limits;
	limits.reserve(lods.size());
	for (int zoom = 0; zoom < lods.size(); ++zoom)
	{
		auto const zoom_factor = qint64(1) << zoom;
		auto const resolution =
			runtime_base_resolution / double(zoom_factor);
		auto const matrix_width =
			base_matrix_width * zoom_factor;
		auto const matrix_height =
			base_matrix_height * zoom_factor;
		matrix_set.matrices.push_back({
			QString::number(zoom),
			zoom,
			resolution,
			QPointF(origin_x, origin_y),
			QSize(int(columns), int(rows)),
			matrix_width,
			matrix_height,
		});

		auto const tile_width = resolution * double(columns);
		auto const tile_height = resolution * double(rows);
		qint64 min_column = 0;
		qint64 max_column = -1;
		qint64 min_row = 0;
		qint64 max_row = -1;
		if (!extentIndex(
			    (west - origin_x) / tile_width,
			    false, 0, matrix_width - 1, &min_column)
		    || !extentIndex(
			    (east - origin_x) / tile_width,
			    true, 0, matrix_width - 1, &max_column)
		    || !extentIndex(
			    (origin_y - north) / tile_height,
			    false, 0, matrix_height - 1, &min_row)
		    || !extentIndex(
			    (origin_y - south) / tile_height,
			    true, 0, matrix_height - 1, &max_row)
		    || min_column > max_column || min_row > max_row)
		{
			return unsupported(tr(
				"ArcGIS fullExtent cannot be represented as tile limits."
			));
		}
		if (zoom >= minimum_zoom && zoom <= maximum_zoom
		    && (min_column != 0 || min_row != 0
		        || max_column != matrix_width - 1
		        || max_row != matrix_height - 1))
		{
			limits.push_back({
				zoom, min_column, max_column, min_row, max_row
			});
		}
	}

	QString matrix_error;
	if (!matrix_set.validateDyadicTopLeft(&matrix_error))
	{
		return unsupported(tr(
			"ArcGIS tile matrices are outside the runtime profile."
		));
	}

	if (!tile_info.value(QStringLiteral("format")).isString()
	    || tile_info.value(QStringLiteral("format"))
		       .toString().trimmed().isEmpty())
	{
		return invalid(tr(
			"ArcGIS cache format is missing or malformed."
		));
	}
	auto const discovered_media_type = mediaType(tile_info);
	if (discovered_media_type.isEmpty())
	{
		return unsupported(tr(
			"ArcGIS cache image format is not supported."
		));
	}

	ArcGisTileServiceResult result;
	result.outcome = ArcGisTileServiceOutcome::Resolved;
	result.service_title = serviceTitle(root, *normalized_url);
	result.likely_secret_parameters =
		ManualImagerySource::likelySecretQueryParameters(*normalized_url);

	ResolvedImagerySource source;
	source.metadata.id = settings.id.isEmpty()
		? generatedId(*normalized_url)
		: settings.id;
	source.metadata.name = settings.name.trimmed().isEmpty()
		? result.service_title
		: settings.name.trimmed();
	source.notices.attribution_text =
		settings.attribution_text.isEmpty()
		? root.value(QStringLiteral("copyrightText")).toString()
		: settings.attribution_text;
	source.notices.attribution_url = settings.attribution_url;
	source.tile_urls = { {
		tileTemplate(*normalized_url)
	} };
	source.row_scheme = TileRowScheme::Xyz;
	source.media_type = discovered_media_type;
	source.tile_matrix_set = std::move(matrix_set);
	source.min_zoom = minimum_zoom;
	source.max_zoom = maximum_zoom;
	source.tile_limits = std::move(limits);
	source.request.referer = settings.referer;
	source.request.empty_http_status_codes =
		settings.empty_http_status_codes;

	QString source_error;
	if (!source.validate(&source_error))
	{
		return invalid(tr(
			"ArcGIS source settings cannot satisfy the runtime requirements."
		));
	}
	result.source = std::move(source);
	return result;
}

}  // namespace OpenOrienteering::imagery
