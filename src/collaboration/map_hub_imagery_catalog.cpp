/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_imagery_catalog.h"

#include <algorithm>

#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

#include "imagery/imagery_catalog_repository.h"
#include "imagery/imagery_catalog_store.h"
#include "imagery/oic_catalog.h"

namespace OpenOrienteering {

namespace {

int effectivePort(const QUrl &url) {
  return url.port(url.scheme() == QLatin1String("https") ? 443 : 80);
}

bool isCredentialFreeMapHubEndpoint(const QUrl &url, const QUrl &manifest_url) {
  return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
         url.query().isEmpty() && url.fragment().isEmpty() &&
         url.scheme().compare(manifest_url.scheme(), Qt::CaseInsensitive) ==
             0 &&
         url.host().compare(manifest_url.host(), Qt::CaseInsensitive) == 0 &&
         effectivePort(url) == effectivePort(manifest_url);
}

QString catalogId(const QString &project_id) {
  auto stable = project_id.toLower();
  stable.remove(QLatin1Char('{'));
  stable.remove(QLatin1Char('}'));
  return QStringLiteral("org.cascadeoc.maphub.project.%1").arg(stable);
}

} // namespace

QJsonObject MapHubImageryCatalog::catalogDocument(const QJsonObject &manifest,
                                                  const QString &manifest_url,
                                                  QString *error) {
  auto project_id = manifest.value(QStringLiteral("id")).toString();
  if (project_id.isEmpty()) {
    if (error)
      *error = QStringLiteral("The project manifest has no stable project ID.");
    return {};
  }
  QJsonArray sources;
  auto trusted_manifest_url = QUrl(manifest_url);
  for (const auto value :
       manifest.value(QStringLiteral("tile_layers")).toArray()) {
    auto layer = value.toObject();
    if (layer.value(QStringLiteral("type")).toString() !=
        QLatin1String("raster"))
      continue;
    auto template_text = layer.value(QStringLiteral("url_template")).toString();
    auto template_url = QUrl(template_text);
    if (!isCredentialFreeMapHubEndpoint(template_url, trusted_manifest_url)) {
      if (error)
        *error = QStringLiteral(
                     "Tile layer “%1” is not a credential-free endpoint on "
                     "the exact Map Hub origin.")
                     .arg(layer.value(QStringLiteral("title")).toString());
      return {};
    }
    QJsonObject source{
        {QStringLiteral("id"), layer.value(QStringLiteral("id")).toString()},
        {QStringLiteral("name"),
         layer.value(QStringLiteral("title")).toString()},
        {QStringLiteral("type"), QStringLiteral("raster-tiles")},
        {QStringLiteral("tiles"), QJsonArray{template_text}},
        {QStringLiteral("scheme"), QStringLiteral("xyz")},
        {QStringLiteral("tileMatrixSetURI"),
         QStringLiteral("http://www.opengis.net/def/tilematrixset/OGC/1.0/"
                        "WebMercatorQuad")},
        {QStringLiteral("minTileMatrix"),
         QString::number(layer.value(QStringLiteral("min_zoom")).toInt())},
        {QStringLiteral("maxTileMatrix"),
         QString::number(layer.value(QStringLiteral("max_zoom")).toInt(22))},
    };
    auto attribution = layer.value(QStringLiteral("attribution")).toString();
    if (!attribution.isEmpty())
      source.insert(
          QStringLiteral("notices"),
          QJsonObject{{QStringLiteral("attributionText"), attribution}});
    auto tile_matrix_limits =
        layer.value(QStringLiteral("tile_matrix_limits")).toArray();
    if (!tile_matrix_limits.isEmpty())
      source.insert(QStringLiteral("tileMatrixLimits"), tile_matrix_limits);
    auto source_raster = layer.value(QStringLiteral("source_raster")).toObject();
    if (!source_raster.isEmpty())
      source.insert(
          QStringLiteral("extensions"),
          QJsonObject{{QStringLiteral("org.cascadeoc.maphub"),
                       QJsonObject{{QStringLiteral("sourceRaster"),
                                    source_raster}}}});
    sources.append(source);
  }
  if (sources.isEmpty())
    return {};
  auto current_revision = manifest.value(QStringLiteral("current_revision"))
                              .toObject()
                              .value(QStringLiteral("number"))
                              .toInt(1);
  return {
      {QStringLiteral("format"),
       QStringLiteral("org.openorienteering.imagery-catalog")},
      {QStringLiteral("version"), 1},
      {QStringLiteral("id"), catalogId(project_id)},
      {QStringLiteral("revision"), std::max(1, current_revision)},
      {QStringLiteral("name"),
       QStringLiteral("Map Hub — %1")
           .arg(manifest.value(QStringLiteral("title")).toString())},
      {QStringLiteral("description"),
       QStringLiteral(
           "Project-authorized tiled sources synchronized from Map Hub.")},
      {QStringLiteral("sources"), sources},
      {QStringLiteral("extensions"),
       QJsonObject{
           {QStringLiteral("org.cascadeoc.maphub"),
            QJsonObject{{QStringLiteral("projectId"), project_id}}},
       }},
  };
}

MapHubImageryCatalogResult
MapHubImageryCatalog::install(const QJsonObject &manifest,
                              const QString &manifest_url) {
  MapHubImageryCatalogResult result;
  QString conversion_error;
  auto document = catalogDocument(manifest, manifest_url, &conversion_error);
  if (!conversion_error.isEmpty()) {
    result.error = conversion_error;
    return result;
  }
  if (document.isEmpty())
    return result;
  auto read_result = imagery::OicCatalogReader::read(
      QJsonDocument(document).toJson(QJsonDocument::Compact));
  if (!read_result.accepted() || read_result.supportedSourceCount() == 0) {
    result.error =
        QStringLiteral("Map Hub tile layers could not be represented by the "
                       "installed tiled-imagery runtime.");
    return result;
  }
  imagery::ImageryCatalogStore store;
  imagery::ImageryCatalogInstallMetadata metadata;
  metadata.origin = manifest_url;
  metadata.final_url = manifest_url;
  imagery::ImageryCatalogInstallOptions options;
  options.allow_lower_revision = true;
  options.allow_same_revision_conflict = true;
  QString install_error;
  if (!store.install(read_result, metadata, options, &install_error)) {
    result.error = install_error;
    return result;
  }
  result.catalog_id = read_result.catalog.id;
  result.installed_sources = read_result.supportedSourceCount();
  imagery::ImageryCatalogRepository::instance().reload();
  return result;
}

} // namespace OpenOrienteering
