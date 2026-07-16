/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    any later version.
 */

#ifndef OPENORIENTEERING_IMAGERY_SOURCE_H
#define OPENORIENTEERING_IMAGERY_SOURCE_H

#include <optional>

#include <QByteArray>
#include <QDate>
#include <QSize>
#include <QString>
#include <QUrl>
#include <QVector>

#include "imagery/tile_matrix_set.h"

namespace OpenOrienteering::imagery {

#ifdef Q_OS_ANDROID
inline constexpr qint64 maximum_runtime_tile_pixels =
	qint64(1) * 1024 * 1024;
inline constexpr int maximum_runtime_tile_dimension = 2048;
#else
inline constexpr qint64 maximum_runtime_tile_pixels =
	qint64(4) * 1024 * 1024;
inline constexpr int maximum_runtime_tile_dimension = 4096;
#endif

inline bool runtimeSupportsTileSize(const QSize& size) noexcept
{
	return size.width() > 0 && size.height() > 0
	       && size.width() <= maximum_runtime_tile_dimension
	       && size.height() <= maximum_runtime_tile_dimension
	       && qint64(size.width()) * size.height()
	            <= maximum_runtime_tile_pixels;
}

enum class ImageryCategory
{
	Aerial,
	Satellite,
	Map,
	Elevation,
	Other,
};

enum class TileRowScheme
{
	Xyz,
	Tms,
};

QString categoryName(ImageryCategory category);
std::optional<ImageryCategory> categoryFromName(const QString& name);
QString tileRowSchemeName(TileRowScheme scheme);
std::optional<TileRowScheme> tileRowSchemeFromName(const QString& name);

struct ImageryMetadata
{
	QString id;
	QString name;
	QString description;
	ImageryCategory category = ImageryCategory::Other;
	QDate start_date;
	QDate end_date;

	bool operator==(const ImageryMetadata&) const = default;
};

struct ImageryNotices
{
	QString attribution_text;
	QUrl attribution_url;
	QUrl source_url;
	QUrl terms_url;
	QUrl privacy_url;
	QString notes;

	bool operator==(const ImageryNotices&) const = default;
};

struct ImageryRequestPolicy
{
	QUrl referer;
	QVector<int> empty_http_status_codes { 204, 404 };

	bool operator==(const ImageryRequestPolicy&) const = default;
};

/**
 * Identity of the installed catalog snapshot from which a source was resolved.
 *
 * This is intentionally distinct from surveyed registration provenance.
 * Embedding it in a map permits an explicit future update comparison without
 * making the map depend on a mutable catalog installation.
 */
struct CatalogSourceProvenance
{
	QString catalog_id;
	int catalog_revision = 0;
	QByteArray catalog_sha256;
	QString source_id;
	QByteArray full_fingerprint;
	QByteArray operational_fingerprint;

	bool operator==(const CatalogSourceProvenance&) const = default;
};

struct ImageryProvenance
{
	QString method;
	QDate observed;
	QString author;
	std::optional<double> rms_error;
	QString notes;

	bool operator==(const ImageryProvenance&) const = default;
};

/**
 * The only registration operation executable by the resolved runtime.
 *
 * Direction and units are intentionally typed rather than configurable:
 * source-to-corrected, with dx/dy in the shared CRS linear unit.
 */
struct TranslationRegistration
{
	QString source_crs;
	QString target_crs;
	QString target_frame_id;
	double dx = 0;
	double dy = 0;
	ImageryProvenance provenance;

	bool operator==(const TranslationRegistration&) const = default;
};

struct TileUrlTemplate
{
	QString value;

	bool validate(QString* error = nullptr) const;
	QUrl expand(const TileMatrix& matrix,
	            qint64 column,
	            qint64 canonical_top_row,
	            TileRowScheme scheme,
	            QString* error = nullptr) const;

	bool operator==(const TileUrlTemplate&) const = default;
};

/**
 * A fully resolved, self-contained source ready for request scheduling.
 *
 * This model deliberately cannot carry affine or grid-shift registration.
 * Catalog definitions requiring those operations must remain disabled until a
 * runtime with explicit support resolves them.
 */
struct ResolvedImagerySource
{
	ImageryMetadata metadata;
	ImageryNotices notices;
	QVector<TileUrlTemplate> tile_urls;
	TileRowScheme row_scheme = TileRowScheme::Xyz;
	QString media_type = QStringLiteral("image/png");
	TileMatrixSet tile_matrix_set;
	int min_zoom = 0;
	int max_zoom = -1;
	QVector<TileMatrixLimits> tile_limits;
	ImageryRequestPolicy request;
	std::optional<CatalogSourceProvenance> catalog_provenance;
	std::optional<TranslationRegistration> registration;

	bool validate(QString* error = nullptr) const;
	const TileMatrixLimits* limitsForZoom(int zoom) const noexcept;
	QUrl tileUrl(int template_index,
	             int zoom,
	             qint64 column,
	             qint64 canonical_top_row,
	             QString* error = nullptr) const;

	bool operator==(const ResolvedImagerySource&) const = default;
};

}  // namespace OpenOrienteering::imagery

#endif
