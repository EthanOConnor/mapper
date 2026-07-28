/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_entity_index.h"

#include <algorithm>
#include <tuple>

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include "collaboration/oom_json.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "core/symbols/symbol.h"

namespace OpenOrienteering {

namespace {

constexpr auto zero_hash =
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr qsizetype maximum_entities = 250'000;
constexpr qsizetype maximum_bytes = 16 * 1024 * 1024;

bool canonicalUuid(const QString &value) {
  const QUuid uuid(value);
  return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == value;
}

int kindOrder(const QString &kind) {
  if (kind == QLatin1String("part"))
    return 0;
  if (kind == QLatin1String("symbol"))
    return 1;
  if (kind == QLatin1String("object"))
    return 2;
  return 3;
}

} // namespace

MapHubEntityIndex MapHubEntityIndex::bootstrap(const Map &map) {
  MapHubEntityIndex index;
  index.stream_hash = QString::fromLatin1(zero_hash);
  QString previous;
  for (int p = 0; p < map.getNumParts(); ++p) {
    const auto *part = map.getPart(p);
    index.entities.push_back(
        {QStringLiteral("part"), part->persistentId(), 1, false, {}, previous});
    previous = part->persistentId();
  }
  previous.clear();
  for (int s = 0; s < map.getNumSymbols(); ++s) {
    const auto *symbol = map.getSymbol(s);
    index.entities.push_back({QStringLiteral("symbol"),
                              symbol->persistentId(),
                              1,
                              false,
                              {},
                              previous});
    previous = symbol->persistentId();
  }
  for (int p = 0; p < map.getNumParts(); ++p) {
    const auto *part = map.getPart(p);
    previous.clear();
    for (int o = 0; o < part->getNumObjects(); ++o) {
      const auto *object = part->getObject(o);
      index.entities.push_back({QStringLiteral("object"),
                                object->persistentId(), 1, false,
                                part->persistentId(), previous});
      previous = object->persistentId();
    }
  }
  std::sort(index.entities.begin(), index.entities.end(),
            [](const auto &left, const auto &right) {
              return std::tuple(kindOrder(left.kind), left.id) <
                     std::tuple(kindOrder(right.kind), right.id);
            });
  return index;
}

MapHubEntityIndex MapHubEntityIndex::fromJson(const QJsonObject &object,
                                              QString *error) {
  MapHubEntityIndex index;
  if (object.value(QStringLiteral("protocol")).toString() !=
          QLatin1String(protocol) ||
      !object.value(QStringLiteral("entities")).isArray()) {
    if (error)
      *error = QStringLiteral("The Map Hub entity index shape is invalid.");
    return {};
  }
  index.stream_sequence =
      object.value(QStringLiteral("stream_sequence")).toInteger(-1);
  index.stream_hash = object.value(QStringLiteral("stream_hash")).toString();
  for (const auto &value : object.value(QStringLiteral("entities")).toArray()) {
    if (!value.isObject()) {
      if (error)
        *error =
            QStringLiteral("A Map Hub entity index entry is not an object.");
      return {};
    }
    const auto entry = value.toObject();
    if (!entry.value(QStringLiteral("tombstone")).isBool()) {
      if (error)
        *error = QStringLiteral("A Map Hub entity tombstone flag is invalid.");
      return {};
    }
    MapHubEntityIndexEntry parsed{
        entry.value(QStringLiteral("kind")).toString(),
        entry.value(QStringLiteral("id")).toString(),
        entry.value(QStringLiteral("version")).toInteger(-1),
        entry.value(QStringLiteral("tombstone")).toBool(),
        {},
        {},
    };
    if (!entry.value(QStringLiteral("parent_id")).isNull())
      parsed.parent_id = entry.value(QStringLiteral("parent_id")).toString();
    if (!entry.value(QStringLiteral("after_id")).isNull())
      parsed.after_id = entry.value(QStringLiteral("after_id")).toString();
    index.entities.push_back(std::move(parsed));
  }
  if (!index.isValid(error))
    return {};
  return index;
}

MapHubEntityIndex MapHubEntityIndex::fromCanonicalBytes(const QByteArray &bytes,
                                                        QString *error) {
  if (bytes.isEmpty() || bytes.size() > maximum_bytes) {
    if (error)
      *error = QStringLiteral("The Map Hub entity index size is invalid.");
    return {};
  }
  QJsonParseError parse_error;
  const auto document = QJsonDocument::fromJson(bytes, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    if (error)
      *error = QStringLiteral("The Map Hub entity index is not valid JSON.");
    return {};
  }
  auto index = fromJson(document.object(), error);
  if (!index.isValid(error) || index.canonicalBytes(error) != bytes) {
    if (error && error->isEmpty())
      *error =
          QStringLiteral("The Map Hub entity index is not canonical JSON.");
    return {};
  }
  return index;
}

bool MapHubEntityIndex::isValid(QString *error) const {
  static const QRegularExpression hash_pattern(
      QStringLiteral("^[0-9a-f]{64}$"));
  if (stream_sequence < 0 || !hash_pattern.match(stream_hash).hasMatch() ||
      entities.size() > maximum_entities) {
    if (error)
      *error = QStringLiteral("The Map Hub entity index envelope is invalid.");
    return false;
  }
  QSet<QString> ids;
  QHash<QString, const MapHubEntityIndexEntry *> by_id;
  QString previous_kind;
  QString previous_id;
  for (const auto &entity : entities) {
    if (kindOrder(entity.kind) > 2 || !canonicalUuid(entity.id) ||
        entity.version < 1 || ids.contains(entity.id) ||
        (!entity.parent_id.isEmpty() && !canonicalUuid(entity.parent_id)) ||
        (!entity.after_id.isEmpty() && !canonicalUuid(entity.after_id))) {
      if (error)
        *error = QStringLiteral("A Map Hub entity index entry is invalid.");
      return false;
    }
    if (!previous_kind.isEmpty() &&
        std::tuple(kindOrder(previous_kind), previous_id) >
            std::tuple(kindOrder(entity.kind), entity.id)) {
      if (error)
        *error = QStringLiteral(
            "The Map Hub entity index is not in canonical order.");
      return false;
    }
    ids.insert(entity.id);
    by_id.insert(entity.id, &entity);
    previous_kind = entity.kind;
    previous_id = entity.id;
  }

  QHash<QString, QVector<const MapHubEntityIndexEntry *>> lists;
  QHash<QString, QSet<QString>> successor_anchors;
  for (const auto &entity : entities) {
    if (entity.tombstone) {
      if (!entity.parent_id.isEmpty() || !entity.after_id.isEmpty()) {
        if (error)
          *error = QStringLiteral(
              "A tombstoned Map Hub entity retains placement metadata.");
        return false;
      }
      continue;
    }
    if ((entity.kind == QLatin1String("object")) !=
        !entity.parent_id.isEmpty()) {
      if (error)
        *error = QStringLiteral(
            "A Map Hub entity index parent relationship is invalid.");
      return false;
    }
    if (!entity.parent_id.isEmpty()) {
      const auto *parent = by_id.value(entity.parent_id);
      if (!parent || parent->kind != QLatin1String("part") ||
          parent->tombstone) {
        if (error)
          *error = QStringLiteral("A Map Hub object has no live map part.");
        return false;
      }
    }
    const auto list_key = entity.kind + QLatin1Char(':') + entity.parent_id;
    lists[list_key].push_back(&entity);
    if (!entity.after_id.isEmpty()) {
      const auto *anchor = by_id.value(entity.after_id);
      if (!anchor || anchor->tombstone || anchor->kind != entity.kind ||
          anchor->parent_id != entity.parent_id ||
          successor_anchors[list_key].contains(entity.after_id)) {
        if (error)
          *error =
              QStringLiteral("A Map Hub entity ordering anchor is invalid.");
        return false;
      }
      successor_anchors[list_key].insert(entity.after_id);
    }
  }
  for (auto list_it = lists.cbegin(); list_it != lists.cend(); ++list_it) {
    const auto &list = list_it.value();
    const MapHubEntityIndexEntry *head = nullptr;
    QHash<QString, const MapHubEntityIndexEntry *> successor;
    for (const auto *entity : list) {
      if (entity->after_id.isEmpty()) {
        if (head) {
          if (error)
            *error =
                QStringLiteral("A Map Hub entity list has multiple heads.");
          return false;
        }
        head = entity;
      } else {
        successor.insert(entity->after_id, entity);
      }
    }
    qsizetype traversed = 0;
    QSet<QString> visited;
    for (auto *entity = head; entity; entity = successor.value(entity->id)) {
      if (visited.contains(entity->id)) {
        if (error)
          *error = QStringLiteral("A Map Hub entity list contains a cycle.");
        return false;
      }
      visited.insert(entity->id);
      ++traversed;
    }
    if (!head || traversed != list.size()) {
      if (error)
        *error = QStringLiteral("A Map Hub entity list is disconnected.");
      return false;
    }
  }
  return true;
}

QByteArray MapHubEntityIndex::canonicalBytes(QString *error) const {
  if (!isValid(error))
    return {};
  QJsonArray entity_array;
  for (const auto &entity : entities) {
    entity_array.append(QJsonObject{
        {QStringLiteral("kind"), entity.kind},
        {QStringLiteral("id"), entity.id},
        {QStringLiteral("version"), entity.version},
        {QStringLiteral("tombstone"), entity.tombstone},
        {QStringLiteral("parent_id"), entity.parent_id.isEmpty()
                                          ? QJsonValue(QJsonValue::Null)
                                          : QJsonValue(entity.parent_id)},
        {QStringLiteral("after_id"), entity.after_id.isEmpty()
                                         ? QJsonValue(QJsonValue::Null)
                                         : QJsonValue(entity.after_id)},
    });
  }
  auto bytes = OomJson::canonical(
      QJsonObject{
          {QStringLiteral("protocol"), QString::fromLatin1(protocol)},
          {QStringLiteral("stream_sequence"), stream_sequence},
          {QStringLiteral("stream_hash"), stream_hash},
          {QStringLiteral("entities"), entity_array},
      },
      error);
  if (bytes.size() > maximum_bytes) {
    if (error)
      *error = QStringLiteral("The Map Hub entity index exceeds 16 MiB.");
    return {};
  }
  return bytes;
}

QString MapHubEntityIndex::sha256(QString *error) const {
  const auto bytes = canonicalBytes(error);
  if (bytes.isEmpty())
    return {};
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool MapHubEntityIndex::matchesMapTopology(const Map &map,
                                           QString *error) const {
  if (!isValid(error))
    return false;
  const auto local = bootstrap(map);
  QHash<QString, MapHubEntityIndexEntry> live;
  for (const auto &entity : entities) {
    if (!entity.tombstone)
      live.insert(entity.id, entity);
  }
  if (local.entities.size() != live.size()) {
    if (error)
      *error = QStringLiteral(
          "The map does not contain the synchronized entity set.");
    return false;
  }
  for (const auto &entity : local.entities) {
    const auto projected = live.constFind(entity.id);
    if (projected == live.cend() || projected->kind != entity.kind ||
        projected->parent_id != entity.parent_id ||
        projected->after_id != entity.after_id) {
      if (error)
        *error =
            QStringLiteral("The map does not match synchronized entity order.");
      return false;
    }
  }
  return true;
}

} // namespace OpenOrienteering
