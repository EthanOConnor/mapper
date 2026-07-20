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

#ifndef OPENORIENTEERING_IMAGERY_MANUAL_IMAGERY_SOURCE_H
#define OPENORIENTEERING_IMAGERY_MANUAL_IMAGERY_SOURCE_H

#include <optional>

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include "imagery/imagery_source.h"

namespace OpenOrienteering::imagery {

enum class ManualImageryOutcome
{
	Direct,
	NeedsDiscovery,
	Unsupported,
	Invalid,
};

enum class ManualImageryInputKind
{
	TiledUrlTemplate,
	ArcGisMapServer,
	ArcGisImageServer,
	Wms,
	Wmts,
	CloudOptimizedGeoTiff,
	Unknown,
};

enum class ManualImageryWarning
{
	/**
	 * Query names look credential-bearing. The complete endpoint will be
	 * embedded in any map snapshot that uses the source.
	 */
	LikelySecretQueryParameters,
};

/**
 * Explicit advanced settings for a direct Web Mercator XYZ/TMS source.
 *
 * The defaults are intentionally represented in the model so the UI can show
 * them rather than applying hidden behavior.
 */
struct ManualTiledSourceSettings
{
	QString id;
	QString name;
	TileRowScheme scheme = TileRowScheme::Xyz;
	int min_zoom = 0;
	int max_zoom = 19;
	int tile_size = 256;
	QString media_type = QStringLiteral("image/png");
	QUrl referer;
	QVector<int> empty_http_status_codes { 204, 404 };
	QString attribution_text;
	QUrl attribution_url;

	bool operator==(const ManualTiledSourceSettings&) const = default;
};

struct ManualImageryDiscoveryResult
{
	ManualImageryOutcome outcome = ManualImageryOutcome::Invalid;
	ManualImageryInputKind input_kind = ManualImageryInputKind::Unknown;
	QString detail;
	QString normalized_template;
	QString suggested_name;
	QUrl service_url;
	QUrl discovery_url;
	QVector<ManualImageryWarning> warnings;
	QStringList likely_secret_parameters;
	std::optional<ResolvedImagerySource> source;

	bool isDirect() const noexcept;

	/**
	 * Manual endpoints may contain credentials and are deliberately never
	 * eligible for recent-source persistence. This does not prevent the
	 * complete endpoint from being embedded in a saved map snapshot.
	 */
	static constexpr bool permitsRecentPersistence() noexcept { return false; }
};

class ManualImagerySource
{
	Q_DECLARE_TR_FUNCTIONS(
		OpenOrienteering::imagery::ManualImagerySource)

public:
	static constexpr int maximum_zoom = 30;

	static ManualImageryDiscoveryResult classify(
		const QString& input,
		const ManualTiledSourceSettings& settings = {}
	);

	static QString normalizeTemplateAliases(QString value);
	static QStringList likelySecretQueryParameters(const QUrl& url);
};

}  // namespace OpenOrienteering::imagery

#endif
