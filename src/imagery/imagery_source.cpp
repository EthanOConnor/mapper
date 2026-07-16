/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "imagery/imagery_source.h"

#include <algorithm>
#include <cmath>

#include <QRegularExpression>
#include <QSet>

namespace OpenOrienteering::imagery {

namespace {

bool fail(QString* error, const QString& message)
{
	if (error)
		*error = message;
	return false;
}

bool containsControl(const QString& value)
{
	for (auto const character : value)
	{
		auto const code = character.unicode();
		if (code < 0x20 || code == 0x7f)
			return true;
	}
	return false;
}

bool validPlainText(const QString& value, qsizetype maximum)
{
	return value.size() <= maximum && !containsControl(value);
}

bool validId(const QString& value)
{
	static const QRegularExpression pattern(
		QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
	);
	return pattern.match(value).hasMatch();
}

bool validMediaType(const QString& value)
{
	static const QRegularExpression pattern(
		QStringLiteral("^image/[A-Za-z0-9!#$&^_.+-]{1,96}$")
	);
	return pattern.match(value).hasMatch();
}

bool validSha256(const QByteArray& value)
{
	if (value.size() != 64)
		return false;
	for (auto const byte : value)
	{
		if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
			return false;
	}
	return true;
}

bool validateHttpUrl(const QUrl& url, QString* error)
{
	if (!url.isValid() || url.isRelative() || url.host().isEmpty())
		return fail(error, QStringLiteral("URL must be absolute and contain a host"));
	auto const scheme = url.scheme().toLower();
	if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
		return fail(error, QStringLiteral("URL must use HTTP or HTTPS"));
	if (!url.userName().isEmpty() || !url.password().isEmpty())
		return fail(error, QStringLiteral("URL user information is not allowed"));
	if (url.hasFragment())
		return fail(error, QStringLiteral("URL fragments are not allowed"));
	auto const text = url.toString(QUrl::FullyEncoded);
	if (text.size() > 8192 || containsControl(text))
		return fail(error, QStringLiteral("URL is too long or contains a control character"));
	if (error)
		error->clear();
	return true;
}

bool validateOptionalHttpUrl(const QUrl& url, QString* error)
{
	if (url.isEmpty())
		return true;
	return validateHttpUrl(url, error);
}

bool validateProvenance(const ImageryProvenance& provenance, QString* error)
{
	if (!validPlainText(provenance.method, 256)
	    || !validPlainText(provenance.author, 512)
	    || !validPlainText(provenance.notes, 4096))
	{
		return fail(error, QStringLiteral("Registration provenance contains invalid text"));
	}
	if (!provenance.observed.isNull() && !provenance.observed.isValid())
		return fail(error, QStringLiteral("Registration provenance date is invalid"));
	if (provenance.rms_error
	    && (!std::isfinite(*provenance.rms_error) || *provenance.rms_error < 0))
	{
		return fail(error, QStringLiteral("Registration RMS error must be finite and nonnegative"));
	}
	return true;
}

qsizetype authorityEnd(const QString& url)
{
	auto const scheme_end = url.indexOf(QStringLiteral("://"));
	if (scheme_end < 0)
		return -1;
	auto result = url.size();
	for (auto const separator : { QLatin1Char('/'), QLatin1Char('?'), QLatin1Char('#') })
	{
		auto const position = url.indexOf(separator, scheme_end + 3);
		if (position >= 0)
			result = std::min(result, position);
	}
	return result;
}

}  // namespace

QString categoryName(ImageryCategory category)
{
	switch (category)
	{
	case ImageryCategory::Aerial: return QStringLiteral("aerial");
	case ImageryCategory::Satellite: return QStringLiteral("satellite");
	case ImageryCategory::Map: return QStringLiteral("map");
	case ImageryCategory::Elevation: return QStringLiteral("elevation");
	case ImageryCategory::Other: return QStringLiteral("other");
	}
	Q_UNREACHABLE_RETURN(QStringLiteral("other"));
}

std::optional<ImageryCategory> categoryFromName(const QString& name)
{
	if (name == QLatin1String("aerial"))
		return ImageryCategory::Aerial;
	if (name == QLatin1String("satellite"))
		return ImageryCategory::Satellite;
	if (name == QLatin1String("map"))
		return ImageryCategory::Map;
	if (name == QLatin1String("elevation"))
		return ImageryCategory::Elevation;
	if (name == QLatin1String("other"))
		return ImageryCategory::Other;
	return std::nullopt;
}

QString tileRowSchemeName(TileRowScheme scheme)
{
	return scheme == TileRowScheme::Tms ? QStringLiteral("tms") : QStringLiteral("xyz");
}

std::optional<TileRowScheme> tileRowSchemeFromName(const QString& name)
{
	if (name == QLatin1String("xyz"))
		return TileRowScheme::Xyz;
	if (name == QLatin1String("tms"))
		return TileRowScheme::Tms;
	return std::nullopt;
}

bool TileUrlTemplate::validate(QString* error) const
{
	if (value.isEmpty() || value.size() > 8192 || containsControl(value))
		return fail(error, QStringLiteral("Tile URL template is empty, too long, or contains a control character"));
	if (value.contains(QStringLiteral("${")))
		return fail(error, QStringLiteral("Tile URL placeholders must use exact {z}, {x}, and {y} spelling"));
	if (value.count(QStringLiteral("{z}")) != 1
	    || value.count(QStringLiteral("{x}")) != 1
	    || value.count(QStringLiteral("{y}")) != 1)
	{
		return fail(error, QStringLiteral("Tile URL template must contain each of {z}, {x}, and {y} exactly once"));
	}

	auto remainder = value;
	remainder.remove(QStringLiteral("{z}"));
	remainder.remove(QStringLiteral("{x}"));
	remainder.remove(QStringLiteral("{y}"));
	if (remainder.contains(QLatin1Char('{')) || remainder.contains(QLatin1Char('}')))
		return fail(error, QStringLiteral("Tile URL template contains an unsupported placeholder"));

	auto const authority_end = authorityEnd(value);
	if (authority_end < 0)
		return fail(error, QStringLiteral("Tile URL template is not an absolute URL"));
	for (auto const& placeholder : {
		QStringLiteral("{z}"), QStringLiteral("{x}"), QStringLiteral("{y}")
	})
	{
		if (value.indexOf(placeholder) < authority_end)
			return fail(error, QStringLiteral("Tile URL placeholders are not allowed in the authority"));
	}

	auto probe = value;
	probe.replace(QStringLiteral("{z}"), QStringLiteral("0"));
	probe.replace(QStringLiteral("{x}"), QStringLiteral("0"));
	probe.replace(QStringLiteral("{y}"), QStringLiteral("0"));
	return validateHttpUrl(QUrl(probe, QUrl::StrictMode), error);
}

QUrl TileUrlTemplate::expand(const TileMatrix& matrix,
	                         qint64 column,
	                         qint64 canonical_top_row,
	                         TileRowScheme scheme,
	                         QString* error) const
{
	if (!validate(error))
		return {};
	if (!matrix.contains(column, canonical_top_row))
	{
		fail(error, QStringLiteral("Tile coordinate falls outside the matrix"));
		return {};
	}

	auto request_row = canonical_top_row;
	if (scheme == TileRowScheme::Tms)
		request_row = matrix.matrix_height - 1 - canonical_top_row;

	auto expanded = value;
	expanded.replace(QStringLiteral("{z}"), matrix.id);
	expanded.replace(QStringLiteral("{x}"), QString::number(column));
	expanded.replace(QStringLiteral("{y}"), QString::number(request_row));
	QUrl result(expanded, QUrl::StrictMode);
	if (!validateHttpUrl(result, error))
		return {};
	return result;
}

bool ResolvedImagerySource::validate(QString* error) const
{
	if (!validId(metadata.id))
		return fail(error, QStringLiteral("Imagery source ID is invalid"));
	if (metadata.name.trimmed().isEmpty() || !validPlainText(metadata.name, 512)
	    || !validPlainText(metadata.description, 4096))
	{
		return fail(error, QStringLiteral("Imagery source metadata contains invalid text"));
	}
	if (!metadata.start_date.isNull() && !metadata.start_date.isValid())
		return fail(error, QStringLiteral("Imagery start date is invalid"));
	if (!metadata.end_date.isNull() && !metadata.end_date.isValid())
		return fail(error, QStringLiteral("Imagery end date is invalid"));
	if (metadata.start_date.isValid() && metadata.end_date.isValid()
	    && metadata.start_date > metadata.end_date)
	{
		return fail(error, QStringLiteral("Imagery start date follows its end date"));
	}

	if (!validPlainText(notices.attribution_text, 2048)
	    || !validPlainText(notices.notes, 4096))
	{
		return fail(error, QStringLiteral("Imagery notices contain invalid text"));
	}
	for (auto const* url : {
		&notices.attribution_url, &notices.source_url,
		&notices.terms_url, &notices.privacy_url
	})
	{
		if (!validateOptionalHttpUrl(*url, error))
			return false;
	}

	if (tile_urls.isEmpty() || tile_urls.size() > 8)
		return fail(error, QStringLiteral("Imagery source must contain between one and eight tile URL templates"));
	QSet<QString> unique_templates;
	for (auto const& tile_url : tile_urls)
	{
		if (!tile_url.validate(error))
			return false;
		if (unique_templates.contains(tile_url.value))
			return fail(error, QStringLiteral("Imagery source contains a duplicate tile URL template"));
		unique_templates.insert(tile_url.value);
	}

	if (!validMediaType(media_type))
		return fail(error, QStringLiteral("Imagery source media type is invalid"));
	if (!tile_matrix_set.validateDyadicTopLeft(error))
		return false;
	if (min_zoom < 0 || max_zoom < min_zoom
	    || !tile_matrix_set.matrixForZoom(min_zoom)
	    || !tile_matrix_set.matrixForZoom(max_zoom))
	{
		return fail(error, QStringLiteral("Imagery source zoom range is invalid"));
	}
	for (auto const& matrix : tile_matrix_set.matrices)
	{
		if (matrix.zoom >= min_zoom && matrix.zoom <= max_zoom
		    && !runtimeSupportsTileSize(matrix.tile_size))
		{
			return fail(
				error,
				QStringLiteral(
					"Imagery tile dimensions exceed the runtime decode profile"));
		}
	}
	if (!validateTileMatrixLimits(tile_limits, tile_matrix_set, error))
		return false;
	for (auto const& limit : tile_limits)
	{
		if (limit.zoom < min_zoom || limit.zoom > max_zoom)
			return fail(error, QStringLiteral("Tile limits fall outside the usable zoom range"));
	}

	if (!validateOptionalHttpUrl(request.referer, error))
		return false;
	QSet<int> status_codes;
	for (auto const status : request.empty_http_status_codes)
	{
		if (status < 100 || status > 599 || status_codes.contains(status))
			return fail(error, QStringLiteral("Empty-tile HTTP status codes must be unique values from 100 through 599"));
		status_codes.insert(status);
	}

	if (catalog_provenance)
	{
		auto const& value = *catalog_provenance;
		if (!validId(value.catalog_id) || !validId(value.source_id)
		    || value.source_id != metadata.id || value.catalog_revision <= 0)
		{
			return fail(error, QStringLiteral("Catalog source provenance identity is invalid"));
		}
		if (!validSha256(value.catalog_sha256)
		    || !validSha256(value.full_fingerprint)
		    || !validSha256(value.operational_fingerprint))
		{
			return fail(error, QStringLiteral("Catalog source provenance fingerprints must be lowercase SHA-256"));
		}
	}

	if (registration)
	{
		auto const& value = *registration;
		if (value.source_crs != tile_matrix_set.crs
		    || value.target_crs != tile_matrix_set.crs)
		{
			return fail(error, QStringLiteral("Translation registration frames must match the tile matrix set CRS"));
		}
		if (!std::isfinite(value.dx) || !std::isfinite(value.dy))
			return fail(error, QStringLiteral("Translation registration must be finite"));
		if (!validPlainText(value.target_frame_id, 256))
			return fail(error, QStringLiteral("Translation target frame ID is invalid"));
		if (!validateProvenance(value.provenance, error))
			return false;
	}

	if (error)
		error->clear();
	return true;
}

const TileMatrixLimits* ResolvedImagerySource::limitsForZoom(int zoom) const noexcept
{
	for (auto const& limit : tile_limits)
	{
		if (limit.zoom == zoom)
			return &limit;
	}
	return nullptr;
}

QUrl ResolvedImagerySource::tileUrl(int template_index,
	                                int zoom,
	                                qint64 column,
	                                qint64 canonical_top_row,
	                                QString* error) const
{
	if (template_index < 0 || template_index >= tile_urls.size())
	{
		fail(error, QStringLiteral("Tile URL template index is out of range"));
		return {};
	}
	if (zoom < min_zoom || zoom > max_zoom)
	{
		fail(error, QStringLiteral("Tile zoom falls outside the usable range"));
		return {};
	}
	auto const* matrix = tile_matrix_set.matrixForZoom(zoom);
	if (!matrix || !matrix->contains(column, canonical_top_row))
	{
		fail(error, QStringLiteral("Tile coordinate falls outside the matrix"));
		return {};
	}
	if (auto const* limits = limitsForZoom(zoom);
	    limits && !limits->contains(column, canonical_top_row))
	{
		fail(error, QStringLiteral("Tile coordinate falls outside source limits"));
		return {};
	}
	return tile_urls.at(template_index).expand(
		*matrix, column, canonical_top_row, row_scheme, error
	);
}

}  // namespace OpenOrienteering::imagery
