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

#ifndef OPENORIENTEERING_IMAGERY_OIC_CATALOG_H
#define OPENORIENTEERING_IMAGERY_OIC_CATALOG_H

#include <optional>

#include <QByteArray>
#include <QDate>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include "imagery/imagery_source.h"

namespace OpenOrienteering::imagery {

enum class OicDiagnosticKind
{
	CatalogError,
	SourceError,
	UnsupportedSource,
};

struct OicDiagnostic
{
	OicDiagnosticKind kind = OicDiagnosticKind::CatalogError;
	QString code;
	QString path;
	QString message;
	int source_index = -1;

	QString displayText() const;
	bool operator==(const OicDiagnostic&) const = default;
};

struct OicPublisher
{
	QString name;
	QUrl url;
	QUrl contact_url;
	QJsonObject original_object;

	bool operator==(const OicPublisher&) const = default;
};

struct OicResource
{
	QString id;
	QString href;
	QString media_type;
	QByteArray sha256;
	qint64 size = 0;
	QJsonObject original_object;

	bool operator==(const OicResource&) const = default;
};

struct OicTileMatrixDefinition
{
	TileMatrix matrix;
	double scale_denominator = 0;
	QString corner_of_origin = QStringLiteral("topLeft");
	bool has_variable_matrix_widths = false;
	QJsonObject original_object;

	bool operator==(const OicTileMatrixDefinition&) const = default;
};

struct OicTileMatrixSetDefinition
{
	TileMatrixSet matrix_set;
	QStringList ordered_axes;
	QVector<OicTileMatrixDefinition> matrices;
	QJsonObject original_object;
	bool dyadic_top_left = false;

	bool operator==(const OicTileMatrixSetDefinition&) const = default;
};

struct OicTileMatrixLimitDefinition
{
	QString tile_matrix;
	qint64 min_row = 0;
	qint64 max_row = -1;
	qint64 min_column = 0;
	qint64 max_column = -1;

	bool operator==(const OicTileMatrixLimitDefinition&) const = default;
};

enum class OicRegistrationKind
{
	None,
	Translation2d,
	Affine2d,
	GridShift,
};

struct OicRegistrationDefinition
{
	OicRegistrationKind kind = OicRegistrationKind::None;
	QString direction;
	QString source_crs;
	QString target_crs;
	QString target_frame_id;
	QString unit;
	double dx = 0;
	double dy = 0;
	double xoff = 0;
	double yoff = 0;
	double s11 = 1;
	double s12 = 0;
	double s21 = 0;
	double s22 = 1;
	QString resource_id;
	QString grid_domain;
	QString grid_crs;
	QString interpolation;
	ImageryProvenance provenance;
	QJsonObject original_object;

	bool operator==(const OicRegistrationDefinition&) const = default;
};

/**
 * One source definition as published in an OIC catalog.
 *
 * valid means that the version-1 definition is structurally sound.
 * supported means that this build can execute every required operation.
 * resolved_source is present when both conditions hold and the containing
 * catalog has no catalog-level errors.
 */
struct OicSourceDefinition
{
	ImageryMetadata metadata;
	ImageryNotices notices;
	QString type;
	QVector<TileUrlTemplate> tile_urls;
	TileRowScheme row_scheme = TileRowScheme::Xyz;
	QString media_type = QStringLiteral("image/png");
	QString min_tile_matrix;
	QString max_tile_matrix;
	QString tile_matrix_set_uri;
	OicTileMatrixSetDefinition tile_matrix_set;
	QVector<OicTileMatrixLimitDefinition> tile_limit_definitions;
	QVector<TileMatrixLimits> tile_limits;
	ImageryRequestPolicy request { QUrl {}, QVector<int> {} };
	OicRegistrationDefinition registration;
	QStringList required_capabilities;
	QStringList unsupported_capabilities;
	QJsonObject coverage;
	QJsonObject extensions;
	QJsonObject original_object;
	QByteArray full_fingerprint;
	QByteArray operational_fingerprint;
	bool valid = false;
	bool supported = false;
	std::optional<ResolvedImagerySource> resolved_source;

	bool operator==(const OicSourceDefinition&) const = default;
};

struct OicCatalog
{
	QString format;
	int version = 0;
	QString id;
	int revision = 0;
	QString name;
	QString description;
	std::optional<OicPublisher> publisher;
	QDate created;
	QDate updated;
	QString catalog_license;
	QStringList required_capabilities;
	QVector<OicResource> resources;
	QVector<OicSourceDefinition> sources;
	QJsonObject extensions;
	QJsonObject original_object;
	QByteArray original_bytes;
	QByteArray document_sha256;

	const OicResource* resource(const QString& id) const noexcept;
	bool operator==(const OicCatalog&) const = default;
};

struct OicCatalogReadResult
{
	OicCatalog catalog;
	QVector<OicDiagnostic> diagnostics;

	bool accepted() const noexcept;
	bool hasCatalogErrors() const noexcept;
	int validSourceCount() const noexcept;
	int supportedSourceCount() const noexcept;
	QVector<ResolvedImagerySource> resolvedSources() const;
};

/**
 * Strict reader for OpenOrienteering Imagery Catalog version 1 JSON.
 *
 * A lexical preflight rejects duplicate members, malformed UTF-8, excessive
 * nesting, nonfinite numbers, and negative zero before Qt's object parser can
 * normalize or discard those distinctions.
 */
class OicCatalogReader
{
public:
	static constexpr qsizetype maximum_document_size = 10 * 1024 * 1024;
	static constexpr int maximum_nesting_depth = 64;
	static constexpr int maximum_string_length = 16 * 1024;
	static constexpr int maximum_url_length = 8192;
	static constexpr int maximum_sources = 1000;
	static constexpr int maximum_resources = 1000;
	static constexpr int maximum_tiles_per_source = 8;
	static constexpr int maximum_tile_matrices = 64;
	static constexpr int maximum_coverage_vertices = 10000;
	static constexpr int maximum_empty_status_codes = 32;

	static QString fileExtension();
	static OicCatalogReadResult read(const QByteArray& bytes);
};

}  // namespace OpenOrienteering::imagery

#endif
