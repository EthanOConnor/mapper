/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_imagery_catalog.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QUrl>

#include "imagery/imagery_catalog_repository.h"

namespace OpenOrienteering {

namespace {

constexpr auto map_hub_extension = "org.cascadeoc.maphub";
constexpr auto web_mercator_quad =
	"http://www.opengis.net/def/tilematrixset/OGC/1.0/WebMercatorQuad";

int effectivePort(const QUrl& url)
{
	return url.port(url.scheme() == QLatin1String("https") ? 443 : 80);
}

bool isCredentialFreeMapHubEndpoint(
	const QUrl& url,
	const QUrl& manifest_url)
{
	return url.isValid() && !url.host().isEmpty()
	       && url.userInfo().isEmpty() && url.query().isEmpty()
	       && url.fragment().isEmpty()
	       && url.scheme().compare(
		       manifest_url.scheme(), Qt::CaseInsensitive) == 0
	       && url.host().compare(
		       manifest_url.host(), Qt::CaseInsensitive) == 0
	       && effectivePort(url) == effectivePort(manifest_url);
}

QString catalogId(const QString& project_id, bool account_catalog = false)
{
	if (account_catalog)
		return QStringLiteral("org.cascadeoc.maphub.account");
	auto stable = project_id.toLower();
	stable.remove(QLatin1Char('{'));
	stable.remove(QLatin1Char('}'));
	return QStringLiteral("org.cascadeoc.maphub.project.%1").arg(stable);
}

bool isLowerHexSha256(const QString& value)
{
	if (value.size() != 64)
		return false;
	return std::all_of(value.cbegin(), value.cend(), [](QChar character) {
		return character.isDigit()
		       || (character >= QLatin1Char('a')
		           && character <= QLatin1Char('f'));
	});
}

QString matrixSetUri(const QJsonObject& layer)
{
	auto uri = layer.value(QStringLiteral("tile_matrix_set_uri")).toString();
	if (!uri.isEmpty())
		return uri;
	auto value = layer.value(QStringLiteral("tile_matrix_set"));
	if (!value.isString())
		return {};
	auto name = value.toString();
	if (name.compare(QLatin1String("WebMercatorQuad"), Qt::CaseInsensitive) == 0)
		return QString::fromLatin1(web_mercator_quad);
	return name;
}

QJsonObject matrixSetObject(const QJsonObject& layer)
{
	auto const value = layer.value(QStringLiteral("tile_matrix_set"));
	return value.isObject() ? value.toObject() : QJsonObject {};
}

QJsonObject bboxMetadata(const QJsonObject& layer)
{
	QJsonObject result;
	if (layer.value(QStringLiteral("coverage_bbox")).isArray())
		result.insert(
			QStringLiteral("coverageBbox"),
			layer.value(QStringLiteral("coverage_bbox")));
	if (layer.value(QStringLiteral("coverage_crs")).isString())
		result.insert(
			QStringLiteral("coverageCrs"),
			layer.value(QStringLiteral("coverage_crs")));
	return result;
}

QJsonObject coverageGeometry(const QJsonObject& layer)
{
	if (layer.value(QStringLiteral("coverage")).isObject())
		return layer.value(QStringLiteral("coverage")).toObject();
	auto const bbox = layer.value(QStringLiteral("coverage_bbox")).toArray();
	if (bbox.size() != 4)
		return {};
	auto const west = bbox.at(0);
	auto const south = bbox.at(1);
	auto const east = bbox.at(2);
	auto const north = bbox.at(3);
	if (!west.isDouble() || !south.isDouble()
	    || !east.isDouble() || !north.isDouble())
		return {};
	return {
		{ QStringLiteral("type"), QStringLiteral("Polygon") },
		{ QStringLiteral("coordinates"), QJsonArray { QJsonArray {
			QJsonArray { west, south }, QJsonArray { east, south },
			QJsonArray { east, north }, QJsonArray { west, north },
			QJsonArray { west, south },
		} } },
	};
}

int catalogRevision(const QJsonObject& manifest)
{
	QJsonObject identity {
		{ QStringLiteral("generation"),
		  manifest.value(QStringLiteral("manifest_generation")) },
		{ QStringLiteral("tile_layers"),
		  manifest.value(QStringLiteral("tile_layers")) },
	};
	auto const digest = QCryptographicHash::hash(
		QJsonDocument(identity).toJson(QJsonDocument::Compact),
		QCryptographicHash::Sha256);
	quint32 value = 0;
	for (int index = 0; index < 4; ++index)
		value = (value << 8) | quint8(digest.at(index));
	return std::max(1, int(value & 0x7fffffffU));
}

bool addMatrixSet(
	QJsonObject& source,
	const QJsonObject& layer,
	QString* error)
{
	auto const inline_set = matrixSetObject(layer);
	auto const uri = matrixSetUri(layer);
	if (!inline_set.isEmpty() && !uri.isEmpty())
	{
		if (error)
			*error = QStringLiteral(
				"Tile layer “%1” publishes two TileMatrixSets.")
				.arg(layer.value(QStringLiteral("title")).toString());
		return false;
	}
	if (!inline_set.isEmpty())
		source.insert(QStringLiteral("tileMatrixSet"), inline_set);
	else if (!uri.isEmpty())
		source.insert(QStringLiteral("tileMatrixSetURI"), uri);
	else
	{
		if (error)
			*error = QStringLiteral(
				"Tile layer “%1” does not publish an executable "
				"TileMatrixSet. Mapper will not guess a CRS.")
				.arg(layer.value(QStringLiteral("title")).toString());
		return false;
	}
	return true;
}

}  // namespace


QJsonObject MapHubImageryCatalog::catalogDocument(
	const QJsonObject& manifest,
	const QString& manifest_url,
	QString* error)
{
	auto const project_id = manifest.value(QStringLiteral("id")).toString();
	auto const account_catalog =
		manifest.value(QStringLiteral("_map_hub_account_catalog")).toBool();
	if (project_id.isEmpty())
	{
		if (error)
			*error = QStringLiteral("The project manifest has no stable project ID.");
		return {};
	}
	auto const trusted_manifest_url = QUrl(manifest_url);
	if (!trusted_manifest_url.isValid()
	    || trusted_manifest_url.scheme().toLower() != QLatin1String("https")
	    || trusted_manifest_url.host().isEmpty())
	{
		if (error)
			*error = QStringLiteral("The Map Hub manifest origin is not trusted.");
		return {};
	}

	QJsonArray sources;
	for (auto const& value : manifest.value(QStringLiteral("tile_layers")).toArray())
	{
		auto const layer = value.toObject();
		if (layer.value(QStringLiteral("type")).toString()
		    != QLatin1String("raster"))
			continue;
		auto const layer_id = layer.value(QStringLiteral("id")).toString();
		auto const generation_token =
			layer.value(QStringLiteral("generation_token")).toString();
		auto const template_text =
			layer.value(QStringLiteral("url_template")).toString();
		if (layer_id.isEmpty())
		{
			if (error)
				*error = QStringLiteral("A Map Hub tile layer has no stable ID.");
			return {};
		}
		if (!isLowerHexSha256(generation_token))
		{
			if (error)
				*error = QStringLiteral(
					"Tile layer “%1” has no verifiable generation token.")
					.arg(layer.value(QStringLiteral("title")).toString());
			return {};
		}
		auto const published_project_id =
			layer.value(QStringLiteral("project_id")).toString();
		if (!published_project_id.isEmpty()
		    && published_project_id != project_id)
		{
			if (error)
				*error = QStringLiteral(
					"A Map Hub imagery layer belongs to a different project.");
			return {};
		}
		if (!isCredentialFreeMapHubEndpoint(QUrl(template_text), trusted_manifest_url))
		{
			if (error)
				*error = QStringLiteral(
					"Tile layer “%1” is not a credential-free endpoint on "
					"the exact Map Hub origin.")
					.arg(layer.value(QStringLiteral("title")).toString());
			return {};
		}
		auto const format = layer.value(QStringLiteral("format")).toString();
		if (!format.startsWith(QLatin1String("image/")))
		{
			if (error)
				*error = QStringLiteral(
					"Tile layer “%1” does not publish its image format.")
					.arg(layer.value(QStringLiteral("title")).toString());
			return {};
		}

		QJsonObject source {
			{ QStringLiteral("id"), layer_id },
			{ QStringLiteral("name"),
			  layer.value(QStringLiteral("title")).toString() },
			{ QStringLiteral("type"), QStringLiteral("raster-tiles") },
			{ QStringLiteral("tiles"), QJsonArray { template_text } },
			{ QStringLiteral("scheme"),
			  layer.value(QStringLiteral("scheme")).toString(QStringLiteral("xyz")) },
			{ QStringLiteral("format"), format },
			{ QStringLiteral("minTileMatrix"),
			  QString::number(layer.value(QStringLiteral("min_zoom")).toInt()) },
			{ QStringLiteral("maxTileMatrix"),
			  QString::number(layer.value(QStringLiteral("max_zoom")).toInt()) },
		};
		auto const description =
			layer.value(QStringLiteral("description")).toString();
		if (!description.isEmpty())
			source.insert(QStringLiteral("description"), description);
		if (!addMatrixSet(source, layer, error))
			return {};

		auto const limits = layer.value(QStringLiteral("tile_matrix_limits")).toArray();
		if (!limits.isEmpty())
			source.insert(QStringLiteral("tileMatrixLimits"), limits);
		auto const coverage = coverageGeometry(layer);
		if (!coverage.isEmpty())
		{
			if (layer.value(QStringLiteral("coverage_crs")).toString()
			    != QLatin1String("EPSG:4326"))
			{
				if (error)
					*error = QStringLiteral(
						"Tile layer “%1” does not identify its coverage as WGS84.")
						.arg(layer.value(QStringLiteral("title")).toString());
				return {};
			}
			source.insert(QStringLiteral("coverage"), coverage);
		}
		for (auto const& pair : {
			std::pair { "category", "category" },
			std::pair { "start_date", "startDate" },
			std::pair { "end_date", "endDate" },
		})
		{
			auto const value = layer.value(QString::fromLatin1(pair.first));
			if (value.isString() && !value.toString().isEmpty())
				source.insert(QString::fromLatin1(pair.second), value);
		}

		QJsonObject notices;
		auto const attribution = layer.value(QStringLiteral("attribution")).toString();
		if (!attribution.isEmpty())
			notices.insert(QStringLiteral("attributionText"), attribution);
		for (auto const& pair : {
			std::pair { "attribution_url", "attributionUrl" },
			std::pair { "source_url", "sourceUrl" },
			std::pair { "terms_url", "termsUrl" },
		})
		{
			auto const value = layer.value(QString::fromLatin1(pair.first));
			if (value.isString() && !value.toString().isEmpty())
				notices.insert(QString::fromLatin1(pair.second), value);
		}
		if (!notices.isEmpty())
			source.insert(QStringLiteral("notices"), notices);

		QJsonArray empty_statuses =
			layer.value(QStringLiteral("empty_http_status_codes")).toArray();
		if (empty_statuses.isEmpty())
			empty_statuses = QJsonArray { 204, 404 };
		source.insert(
			QStringLiteral("request"),
			QJsonObject { { QStringLiteral("emptyHttpStatusCodes"), empty_statuses } });

		if (layer.value(QStringLiteral("registration")).isObject())
			source.insert(QStringLiteral("registration"),
			              layer.value(QStringLiteral("registration")));
		auto const published_metadata =
			layer.value(QStringLiteral("metadata")).toObject();
		auto source_raster =
			published_metadata.value(QStringLiteral("source_raster"));
		if (source_raster.isUndefined())
			source_raster = layer.value(QStringLiteral("source_raster"));
		auto registration_metadata =
			published_metadata.value(QStringLiteral("registration"));
		if (registration_metadata.isUndefined())
			registration_metadata =
				layer.value(QStringLiteral("imagery_registration"));
		QJsonObject metadata {
			{ QStringLiteral("projectId"),
			  account_catalog
				  ? QJsonValue(QJsonValue::Null)
				  : QJsonValue(project_id) },
			{ QStringLiteral("layerId"), layer_id },
			{ QStringLiteral("generation"),
			  generation_token },
			{ QStringLiteral("resampling"),
			  layer.value(QStringLiteral("resampling")) },
			{ QStringLiteral("sourceRaster"),
			  source_raster },
			{ QStringLiteral("registrationMetadata"),
			  registration_metadata },
		};
		auto const bbox = bboxMetadata(layer);
		for (auto it = bbox.begin(); it != bbox.end(); ++it)
			metadata.insert(it.key(), it.value());
		source.insert(
			QStringLiteral("extensions"),
			QJsonObject { { QString::fromLatin1(map_hub_extension), metadata } });
		sources.append(source);
	}
	if (sources.isEmpty())
		return {};

	return {
		{ QStringLiteral("format"),
		  QStringLiteral("org.openorienteering.imagery-catalog") },
		{ QStringLiteral("version"), 1 },
		{ QStringLiteral("id"), catalogId(project_id, account_catalog) },
		{ QStringLiteral("revision"), catalogRevision(manifest) },
		{ QStringLiteral("name"),
		  QStringLiteral("Map Hub — %1")
			  .arg(manifest.value(QStringLiteral("title")).toString()) },
		{ QStringLiteral("description"),
		  QStringLiteral("Authorized imagery synchronized from Map Hub.") },
		{ QStringLiteral("sources"), sources },
		{ QStringLiteral("extensions"),
		  QJsonObject { { QString::fromLatin1(map_hub_extension),
		                  QJsonObject {
			                  { QStringLiteral("projectId"),
			                    account_catalog
				                    ? QJsonValue(QJsonValue::Null)
				                    : QJsonValue(project_id) },
			                  { QStringLiteral("generation"),
			                    manifest.value(QStringLiteral("manifest_generation")) },
		                  } } } },
	};
}


MapHubImageryCatalogResult MapHubImageryCatalog::install(
	const QJsonObject& manifest,
	const QString& manifest_url,
	imagery::ImageryCatalogRepository* repository)
{
	MapHubImageryCatalogResult result;
	QString conversion_error;
	auto const document = catalogDocument(manifest, manifest_url, &conversion_error);
	if (!conversion_error.isEmpty())
	{
		result.error = conversion_error;
		return result;
	}
	auto& target = repository
		? *repository
		: imagery::ImageryCatalogRepository::instance();
	if (document.isEmpty())
	{
		auto const project_id = manifest.value(QStringLiteral("id")).toString();
		auto const account_catalog =
			manifest.value(QStringLiteral("_map_hub_account_catalog")).toBool();
		if (!project_id.isEmpty())
		{
			result.catalog_id = catalogId(project_id, account_catalog);
			result.operation_id = target.removeCatalog(result.catalog_id);
		}
		return result;
	}
	imagery::ImageryCatalogInstallMetadata metadata;
	metadata.origin = manifest_url;
	metadata.final_url = manifest_url;
	imagery::ImageryCatalogInstallOptions options;
	options.allow_lower_revision = true;
	options.allow_same_revision_conflict = true;
	result.catalog_id = document.value(QStringLiteral("id")).toString();
	result.installed_sources = document.value(QStringLiteral("sources")).toArray().size();
	result.operation_id = target.installCatalogBytes(
		QJsonDocument(document).toJson(QJsonDocument::Compact),
		std::move(metadata), options);
	return result;
}


MapHubImageryCatalogBatchResult
MapHubImageryCatalog::installAuthorizedCatalog(
	const QJsonObject& catalog,
	const QString& catalog_url,
	imagery::ImageryCatalogRepository* repository)
{
	MapHubImageryCatalogBatchResult batch;
	if (catalog.value(QStringLiteral("schema_version")).toInt() != 1)
	{
		batch.error = QStringLiteral(
			"Map Hub returned an unsupported imagery catalog version.");
		return batch;
	}
	auto const generation =
		catalog.value(QStringLiteral("catalog_generation")).toString();
	if (!isLowerHexSha256(generation))
	{
		batch.error = QStringLiteral(
			"Map Hub returned an imagery catalog without a verifiable generation.");
		return batch;
	}

	constexpr auto account_key = "__map_hub_account__";
	QMap<QString, QJsonArray> project_layers;
	QMap<QString, QString> project_titles;
	for (auto const& value : catalog.value(QStringLiteral("layers")).toArray())
	{
		auto const layer = value.toObject();
		auto const project_id =
			layer.value(QStringLiteral("project_id")).toString();
		auto const grouping_id = project_id.isEmpty()
			? QString::fromLatin1(account_key)
			: project_id;
		project_layers[grouping_id].append(layer);
		project_titles[grouping_id] = project_id.isEmpty()
			? QStringLiteral("Shared imagery")
			:
			layer.value(QStringLiteral("project_title")).toString();
	}

	auto& target = repository
		? *repository
		: imagery::ImageryCatalogRepository::instance();
	QSet<QString> desired_catalogs;
	struct PendingCatalog
	{
		QJsonObject document;
		QString catalog_id;
		int source_count = 0;
	};
	QVector<PendingCatalog> pending_catalogs;
	for (auto it = project_layers.cbegin(); it != project_layers.cend(); ++it)
	{
		auto const group_generation = QString::fromLatin1(
			QCryptographicHash::hash(
				QJsonDocument(it.value()).toJson(QJsonDocument::Compact),
				QCryptographicHash::Sha256)
				.toHex());
		QJsonObject manifest {
			{ QStringLiteral("id"),
			  it.key() == QLatin1String(account_key)
				  ? QStringLiteral("account")
				  : it.key() },
			{ QStringLiteral("title"), project_titles.value(it.key()) },
			{ QStringLiteral("manifest_generation"), group_generation },
			{ QStringLiteral("tile_layers"), it.value() },
		};
		if (it.key() == QLatin1String(account_key))
			manifest.insert(QStringLiteral("_map_hub_account_catalog"), true);
		QString conversion_error;
		auto document = catalogDocument(
			manifest, catalog_url, &conversion_error);
		if (!conversion_error.isEmpty())
		{
			batch.error = conversion_error;
			return batch;
		}
		if (document.isEmpty())
			continue;
		auto const id = document.value(QStringLiteral("id")).toString();
		auto const source_count = int(
			document.value(QStringLiteral("sources")).toArray().size());
		desired_catalogs.insert(id);
		pending_catalogs.push_back({ std::move(document), id, source_count });
	}

	// Validate the complete response before changing any locally installed
	// catalog. A malformed descriptor must not leave a partially refreshed
	// account catalog behind.
	for (auto& pending : pending_catalogs)
	{
		imagery::ImageryCatalogInstallMetadata metadata;
		metadata.origin = catalog_url;
		metadata.final_url = catalog_url;
		imagery::ImageryCatalogInstallOptions options;
		options.allow_lower_revision = true;
		options.allow_same_revision_conflict = true;
		batch.operation_ids.push_back(target.installCatalogBytes(
			QJsonDocument(pending.document).toJson(QJsonDocument::Compact),
			std::move(metadata), options));
		batch.installed_sources += pending.source_count;
	}

	auto const snapshot = target.snapshot();
	if (snapshot)
	{
		for (auto const& installed : snapshot->catalogs)
		{
			auto const id = installed.read_result.catalog.id;
			if ((id.startsWith(QLatin1String("org.cascadeoc.maphub.project."))
			     || id == QLatin1String("org.cascadeoc.maphub.account"))
			    && !desired_catalogs.contains(id))
			{
				batch.operation_ids.push_back(target.removeCatalog(id));
			}
		}
	}
	return batch;
}

}  // namespace OpenOrienteering
