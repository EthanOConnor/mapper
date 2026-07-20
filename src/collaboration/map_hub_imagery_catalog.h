/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_IMAGERY_CATALOG_H
#define OPENORIENTEERING_MAP_HUB_IMAGERY_CATALOG_H

#include <QJsonObject>
#include <QString>

namespace OpenOrienteering {

struct MapHubImageryCatalogResult {
  QString catalog_id;
  int installed_sources = 0;
  QString error;

  explicit operator bool() const { return error.isEmpty(); }
};

/**
 * Converts a trusted project manifest's raster layers into an immutable OIC
 * catalog snapshot used by the tiled-imagery subsystem.
 */
class MapHubImageryCatalog {
public:
  static QJsonObject catalogDocument(const QJsonObject &manifest,
                                     const QString &manifest_url,
                                     QString *error = nullptr);
  static MapHubImageryCatalogResult install(const QJsonObject &manifest,
                                            const QString &manifest_url);
};

} // namespace OpenOrienteering

#endif
