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

#ifndef OPENORIENTEERING_IMAGERY_ARCGIS_TILE_SERVICE_H
#define OPENORIENTEERING_IMAGERY_ARCGIS_TILE_SERVICE_H

#include <optional>

#include <QByteArray>
#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include "imagery/imagery_source.h"

namespace OpenOrienteering::imagery {

enum class ArcGisTileServiceOutcome
{
	Resolved,
	Unsupported,
	Invalid,
};

struct ArcGisTileServiceSettings
{
	QString id;
	QString name;
	QUrl referer;
	QVector<int> empty_http_status_codes { 204, 404 };
	QString attribution_text;
	QUrl attribution_url;

	bool operator==(const ArcGisTileServiceSettings&) const = default;
};

struct ArcGisTileServiceResult
{
	ArcGisTileServiceOutcome outcome = ArcGisTileServiceOutcome::Invalid;
	QString detail;
	QString service_title;
	QStringList likely_secret_parameters;
	std::optional<ResolvedImagerySource> source;

	bool resolved() const noexcept;
};

/**
 * Pure parser for ArcGIS REST service metadata (`f=pjson`).
 *
 * This class performs no network access. The caller supplies the exact
 * service URL and bounded response bytes obtained through its network policy.
 */
class ArcGisTileService
{
	Q_DECLARE_TR_FUNCTIONS(
		OpenOrienteering::imagery::ArcGisTileService)

public:
	static constexpr qsizetype maximum_metadata_size = 1024 * 1024;
	static constexpr int maximum_lods = 31;

	static ArcGisTileServiceResult parse(
		const QByteArray& pjson,
		const QUrl& service_url,
		const ArcGisTileServiceSettings& settings = {}
	);
};

}  // namespace OpenOrienteering::imagery

#endif
