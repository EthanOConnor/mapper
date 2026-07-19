/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "imagery/manual_imagery_source.h"

#include <algorithm>
#include <cmath>

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>

namespace OpenOrienteering::imagery {

namespace {

constexpr auto arcgis_path_pattern =
	"^(.*/rest/services/.*/(MapServer|ImageServer))"
	"(?:/tile/[^/]+/[^/]+/[^/]+)?/?$";

bool containsWhitespaceOrControl(const QString& value)
{
	for (auto const character : value)
	{
		auto const code = character.unicode();
		if (code < 0x20 || code == 0x7f || character.isSpace())
			return true;
	}
	return false;
}

bool authorityContainsUserInfo(const QString& value)
{
	auto const scheme_end = value.indexOf(QStringLiteral("://"));
	if (scheme_end < 0)
		return false;
	auto authority_end = value.size();
	for (auto const separator : {
		QLatin1Char('/'), QLatin1Char('?'), QLatin1Char('#')
	})
	{
		auto const position =
			value.indexOf(separator, scheme_end + 3);
		if (position >= 0)
			authority_end = std::min(authority_end, position);
	}
	return value.mid(
		scheme_end + 3,
		authority_end - (scheme_end + 3)
	).contains(QLatin1Char('@'));
}

bool validHttpUrl(const QUrl& url)
{
	auto const scheme = url.scheme().toLower();
	return url.isValid() && !url.isRelative() && !url.host().isEmpty()
	       && (scheme == QLatin1String("http")
	           || scheme == QLatin1String("https"))
	       && url.userName().isEmpty() && url.password().isEmpty()
	       && !url.hasFragment();
}

QString compactQueryKey(QString value)
{
	value = value.toLower();
	value.remove(QLatin1Char('-'));
	value.remove(QLatin1Char('_'));
	value.remove(QLatin1Char('.'));
	return value;
}

bool isLikelySecretKey(const QString& key)
{
	static const QSet<QString> names {
		QStringLiteral("token"),
		QStringLiteral("accesstoken"),
		QStringLiteral("apikey"),
		QStringLiteral("key"),
		QStringLiteral("auth"),
		QStringLiteral("authorization"),
		QStringLiteral("signature"),
		QStringLiteral("sig"),
		QStringLiteral("secret"),
		QStringLiteral("clientsecret"),
		QStringLiteral("credential"),
		QStringLiteral("credentials"),
		QStringLiteral("session"),
		QStringLiteral("sessionid"),
		QStringLiteral("jwt"),
		QStringLiteral("password"),
		QStringLiteral("passwd"),
		QStringLiteral("pass"),
		QStringLiteral("subscriptionkey"),
		QStringLiteral("xamzsignature"),
		QStringLiteral("xamzcredential"),
		QStringLiteral("xamzsecuritytoken"),
		QStringLiteral("googsignature"),
		QStringLiteral("googleaccessid"),
	};
	return names.contains(compactQueryKey(key));
}

QString queryService(const QUrl& url)
{
	for (auto const& item :
	     QUrlQuery(url).queryItems(QUrl::FullyDecoded))
	{
		if (item.first.compare(
			    QStringLiteral("service"), Qt::CaseInsensitive) == 0)
		{
			return item.second.trimmed().toLower();
		}
	}
	return {};
}

bool pathHasServiceSegment(
	const QString& path,
	const QString& service)
{
	auto const pattern = QStringLiteral("(?:^|/)%1(?:/|$)")
		.arg(QRegularExpression::escape(service));
	return QRegularExpression(
		pattern, QRegularExpression::CaseInsensitiveOption
	).match(path).hasMatch();
}

QString filteredArcGisQuery(const QUrl& url)
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

QUrl urlProbe(const QString& normalized_template)
{
	auto probe = normalized_template;
	probe.replace(QStringLiteral("{z}"), QStringLiteral("0"));
	probe.replace(QStringLiteral("{x}"), QStringLiteral("0"));
	probe.replace(QStringLiteral("{y}"), QStringLiteral("0"));
	return QUrl(probe, QUrl::StrictMode);
}

QString generatedId(const QString& normalized_template)
{
	auto const digest = QCryptographicHash::hash(
		normalized_template.toUtf8(), QCryptographicHash::Sha256
	).toHex();
	return QStringLiteral("manual-%1")
		.arg(QString::fromLatin1(digest.first(16)));
}

QString suggestedHostName(const QUrl& url)
{
	auto name = url.host().toLower();
	if (name.isEmpty())
	{
		name = QCoreApplication::translate(
			"OpenOrienteering::imagery::ManualImagerySource",
			"Online imagery");
	}
	return name;
}

TileMatrixSet webMercatorMatrixSet(int tile_size, int maximum_zoom)
{
	constexpr auto half_world = 20037508.342789244;
	auto const base_cell_size =
		(2 * half_world) / double(tile_size);

	TileMatrixSet result;
	result.id = tile_size == 256
		? QStringLiteral("WebMercatorQuad")
		: QStringLiteral("WebMercatorQuad512");
	result.crs = QStringLiteral("EPSG:3857");
	result.matrices.reserve(maximum_zoom + 1);
	for (int zoom = 0; zoom <= maximum_zoom; ++zoom)
	{
		auto const dimension = qint64(1) << zoom;
		result.matrices.push_back({
			QString::number(zoom),
			zoom,
			base_cell_size / double(dimension),
			QPointF(-half_world, half_world),
			QSize(tile_size, tile_size),
			dimension,
			dimension,
		});
	}
	return result;
}

void addSecretWarning(
	ManualImageryDiscoveryResult& result,
	const QUrl& url)
{
	result.likely_secret_parameters =
		ManualImagerySource::likelySecretQueryParameters(url);
	if (!result.likely_secret_parameters.isEmpty())
	{
		result.warnings.push_back(
			ManualImageryWarning::LikelySecretQueryParameters
		);
	}
}

bool classifyArcGis(
	const QUrl& probe,
	ManualImageryDiscoveryResult& result)
{
	static const QRegularExpression pattern(
		QString::fromLatin1(arcgis_path_pattern),
		QRegularExpression::CaseInsensitiveOption
	);
	auto const match = pattern.match(probe.path());
	if (!match.hasMatch())
		return false;

	result.outcome = ManualImageryOutcome::NeedsDiscovery;
	result.input_kind =
		match.captured(2).compare(
			QStringLiteral("MapServer"), Qt::CaseInsensitive) == 0
		? ManualImageryInputKind::ArcGisMapServer
		: ManualImageryInputKind::ArcGisImageServer;

	result.service_url = probe;
	result.service_url.setPath(match.captured(1));
	result.service_url.setFragment({});
	auto const service_query = filteredArcGisQuery(probe);
	result.service_url.setQuery(service_query, QUrl::StrictMode);
	auto discovery_text =
		result.service_url.toString(QUrl::FullyEncoded);
	discovery_text += service_query.isEmpty()
		? QStringLiteral("?f=pjson")
		: QStringLiteral("&f=pjson");
	result.discovery_url =
		QUrl(discovery_text, QUrl::StrictMode);

	auto service_path = match.captured(1);
	service_path.chop(match.captured(2).size());
	while (service_path.endsWith(QLatin1Char('/')))
		service_path.chop(1);
	result.suggested_name =
		service_path.section(QLatin1Char('/'), -1);
	if (result.suggested_name.isEmpty())
		result.suggested_name = suggestedHostName(probe);
	result.detail = QCoreApplication::translate(
		"OpenOrienteering::imagery::ManualImagerySource",
		"ArcGIS service metadata is required before this source can be used.");
	return true;
}

}  // namespace

bool ManualImageryDiscoveryResult::isDirect() const noexcept
{
	return outcome == ManualImageryOutcome::Direct
	       && source.has_value();
}

ManualImageryDiscoveryResult ManualImagerySource::classify(
	const QString& input,
	const ManualTiledSourceSettings& settings)
{
	ManualImageryDiscoveryResult result;
	if (input.isEmpty())
	{
		result.detail = tr("Enter an imagery URL.");
		return result;
	}
	if (input != input.trimmed()
	    || input.size() > 8192
	    || containsWhitespaceOrControl(input)
	    || authorityContainsUserInfo(input))
	{
		result.detail = tr(
			"The URL is too long or contains whitespace, controls, or user information."
		);
		return result;
	}

	result.normalized_template = normalizeTemplateAliases(input);
	auto const probe = urlProbe(result.normalized_template);
	if (!validHttpUrl(probe))
	{
		result.detail = tr(
			"Imagery URLs must use HTTP or HTTPS with a host and no fragment."
		);
		return result;
	}
	result.suggested_name = suggestedHostName(probe);
	addSecretWarning(result, probe);

	auto const service = queryService(probe);
	auto const path = probe.path();
	auto const has_xyz_placeholders =
		result.normalized_template.contains(QStringLiteral("{z}"))
		&& result.normalized_template.contains(QStringLiteral("{x}"))
		&& result.normalized_template.contains(QStringLiteral("{y}"));
	if (service == QLatin1String("wms")
	    || (service.isEmpty() && !has_xyz_placeholders
	        && pathHasServiceSegment(path, QStringLiteral("wms"))))
	{
		result.outcome = ManualImageryOutcome::Unsupported;
		result.input_kind = ManualImageryInputKind::Wms;
		result.detail = tr(
			"WMS sources are recognized but are not supported by the tiled raster runtime."
		);
		return result;
	}
	if (service == QLatin1String("wmts")
	    || (service.isEmpty() && !has_xyz_placeholders
	        && pathHasServiceSegment(path, QStringLiteral("wmts"))))
	{
		result.outcome = ManualImageryOutcome::Unsupported;
		result.input_kind = ManualImageryInputKind::Wmts;
		result.detail = tr(
			"WMTS sources are recognized but are not supported by the tiled raster runtime."
		);
		return result;
	}

	if (classifyArcGis(probe, result))
		return result;
	if (pathHasServiceSegment(path, QStringLiteral("MapServer"))
	    || pathHasServiceSegment(path, QStringLiteral("ImageServer")))
	{
		result.input_kind =
			pathHasServiceSegment(path, QStringLiteral("MapServer"))
			? ManualImageryInputKind::ArcGisMapServer
			: ManualImageryInputKind::ArcGisImageServer;
		result.detail = tr(
			"ArcGIS URLs must identify the service root or an exact three-coordinate tile endpoint."
		);
		return result;
	}

	if (!has_xyz_placeholders
	    && QRegularExpression(
		    QStringLiteral("\\.tiff?$"),
		    QRegularExpression::CaseInsensitiveOption
	    ).match(path).hasMatch())
	{
		result.outcome = ManualImageryOutcome::Unsupported;
		result.input_kind =
			ManualImageryInputKind::CloudOptimizedGeoTiff;
		result.detail = tr(
			"Cloud Optimized GeoTIFF sources are recognized but are not supported by the tiled raster runtime."
		);
		return result;
	}

	if (!has_xyz_placeholders)
	{
		result.detail = tr(
			"A direct tiled source must contain {z}, {x}, and {y} placeholders."
		);
		return result;
	}

	result.input_kind = ManualImageryInputKind::TiledUrlTemplate;
	TileUrlTemplate tile_url { result.normalized_template };
	QString error;
	if (!tile_url.validate(&error))
	{
		result.detail = tr(
			"The URL template does not satisfy the tiled source requirements.");
		return result;
	}
	if (settings.min_zoom < 0
	    || settings.max_zoom < settings.min_zoom
	    || settings.max_zoom > maximum_zoom)
	{
		result.detail = tr(
			"The zoom range must be ordered and fall between 0 and %1."
		).arg(maximum_zoom);
		return result;
	}
	if (settings.tile_size != 256 && settings.tile_size != 512)
	{
		result.detail = tr(
			"Direct tiled sources must use 256 or 512 pixel square tiles."
		);
		return result;
	}

	ResolvedImagerySource source;
	source.metadata.id = settings.id.isEmpty()
		? generatedId(result.normalized_template)
		: settings.id;
	source.metadata.name = settings.name.trimmed().isEmpty()
		? result.suggested_name
		: settings.name.trimmed();
	source.notices.attribution_text = settings.attribution_text;
	source.notices.attribution_url = settings.attribution_url;
	source.tile_urls = { std::move(tile_url) };
	source.row_scheme = settings.scheme;
	source.media_type = settings.media_type;
	source.tile_matrix_set =
		webMercatorMatrixSet(settings.tile_size, settings.max_zoom);
	source.min_zoom = settings.min_zoom;
	source.max_zoom = settings.max_zoom;
	source.request.referer = settings.referer;
	source.request.empty_http_status_codes =
		settings.empty_http_status_codes;

	if (!source.validate(&error))
	{
		result.detail = tr(
			"The direct tiled source settings do not satisfy the runtime requirements."
		);
		return result;
	}
	result.outcome = ManualImageryOutcome::Direct;
	result.source = std::move(source);
	result.detail.clear();
	return result;
}

QString ManualImagerySource::normalizeTemplateAliases(QString value)
{
	value.replace(QStringLiteral("${z}"), QStringLiteral("{z}"));
	value.replace(QStringLiteral("${x}"), QStringLiteral("{x}"));
	value.replace(QStringLiteral("${y}"), QStringLiteral("{y}"));
	return value;
}

QStringList ManualImagerySource::likelySecretQueryParameters(
	const QUrl& url)
{
	QStringList result;
	for (auto const& item :
	     QUrlQuery(url).queryItems(QUrl::FullyDecoded))
	{
		if (isLikelySecretKey(item.first)
		    && !result.contains(item.first, Qt::CaseInsensitive))
		{
			result.push_back(item.first);
		}
	}
	std::sort(
		result.begin(), result.end(),
		[](const QString& first, const QString& second) {
			return first.compare(second, Qt::CaseInsensitive) < 0;
		}
	);
	return result;
}

}  // namespace OpenOrienteering::imagery
