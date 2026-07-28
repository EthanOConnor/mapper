/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_ENTITY_INDEX_H
#define OPENORIENTEERING_MAP_HUB_ENTITY_INDEX_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace OpenOrienteering {

class Map;

struct MapHubEntityIndexEntry {
  QString kind;
  QString id;
  qint64 version = 0;
  bool tombstone = false;
  QString parent_id;
  QString after_id;
};

struct MapHubEntityIndex {
  static constexpr auto protocol = "oom-entity-index/1";

  qint64 stream_sequence = 0;
  QString stream_hash;
  QVector<MapHubEntityIndexEntry> entities;

  static MapHubEntityIndex bootstrap(const Map &map);
  static MapHubEntityIndex fromJson(const QJsonObject &object,
                                    QString *error = nullptr);
  static MapHubEntityIndex fromCanonicalBytes(const QByteArray &bytes,
                                              QString *error = nullptr);
  QByteArray canonicalBytes(QString *error = nullptr) const;
  QString sha256(QString *error = nullptr) const;
  bool isValid(QString *error = nullptr) const;
  bool matchesMapTopology(const Map &map, QString *error = nullptr) const;
};

} // namespace OpenOrienteering

#endif
