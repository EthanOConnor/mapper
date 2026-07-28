/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_edit_transaction.h"

#include <algorithm>

#include <QBuffer>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QUuid>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "collaboration/oom_json.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "fileformats/xml_file_format.h"
#include "undo/undo.h"

namespace OpenOrienteering {

namespace {

constexpr qsizetype maximum_operations = 256;
constexpr qsizetype maximum_fragment_bytes = 256 * 1024;
constexpr qsizetype maximum_aggregate_fragment_bytes = 768 * 1024;
constexpr qsizetype maximum_transaction_bytes = 1024 * 1024;

bool canonicalUuid(const QString &value) {
  const QUuid uuid(value);
  return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == value;
}

bool validHash(const QString &value) {
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return pattern.match(value).hasMatch();
}

bool validXmlFragment(const QString &fragment, const QString &root,
                      const QString &expected_id) {
  const auto trimmed = fragment.trimmed();
  if (trimmed.isEmpty() ||
      trimmed.startsWith(QLatin1String("<?xml"), Qt::CaseInsensitive) ||
      trimmed.contains(QLatin1String("<!DOCTYPE"), Qt::CaseInsensitive) ||
      trimmed.contains(QLatin1String("<!ENTITY"), Qt::CaseInsensitive))
    return false;
  QXmlStreamReader xml(fragment);
  if (!xml.readNextStartElement() || xml.name() != root ||
      xml.attributes().value(QLatin1String("uuid")) != expected_id)
    return false;
  xml.skipCurrentElement();
  while (!xml.atEnd())
    xml.readNext();
  return !xml.hasError();
}

QString serializeObject(const Object &object) {
  QByteArray bytes;
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::WriteOnly))
    return {};
  QXmlStreamWriter xml(&buffer);
  xml.setAutoFormatting(false);
  QScopedValueRollback<int> format_version(XMLFileFormat::active_version,
                                           XMLFileFormat::current_version);
  object.save(xml);
  buffer.close();
  return QString::fromUtf8(bytes);
}

struct LocatedObject {
  const Object *object = nullptr;
  const MapPart *part = nullptr;
  int part_index = -1;
  int object_index = -1;
};

LocatedObject locate(const Map &map, const QString &id) {
  for (int p = 0; p < map.getNumParts(); ++p) {
    const auto *part = map.getPart(p);
    for (int o = 0; o < part->getNumObjects(); ++o) {
      const auto *object = part->getObject(o);
      if (object->persistentId() == id)
        return {object, part, p, o};
    }
  }
  return {};
}

} // namespace

QString MapHubEditOperation::entityKind() const {
  switch (kind) {
  case Kind::PutObject:
  case Kind::DeleteObject:
    return QStringLiteral("object");
  case Kind::PutPart:
  case Kind::DeletePart:
    return QStringLiteral("part");
  case Kind::PutSymbol:
  case Kind::DeleteSymbol:
    return QStringLiteral("symbol");
  }
  return {};
}

bool MapHubEditOperation::isDelete() const {
  return kind == Kind::DeleteObject || kind == Kind::DeletePart ||
         kind == Kind::DeleteSymbol;
}

QJsonObject MapHubEditOperation::toJson() const {
  const auto entity_kind = entityKind();
  if (isDelete()) {
    QJsonObject object;
    object.insert(QStringLiteral("op"),
                  QString(entity_kind + QStringLiteral(".delete")));
    object.insert(QStringLiteral("v"), 1);
    object.insert(QStringLiteral("expected_version"), expected_version);
    object.insert(entity_kind + QStringLiteral("_id"), entity_id);
    return object;
  }
  if (kind == Kind::PutPart) {
    return {
        {QStringLiteral("op"), QStringLiteral("part.put")},
        {QStringLiteral("v"), 1},
        {QStringLiteral("part_id"), entity_id},
        {QStringLiteral("after_part_id"), after_id.isEmpty()
                                              ? QJsonValue(QJsonValue::Null)
                                              : QJsonValue(after_id)},
        {QStringLiteral("expected_version"), expected_version},
        {QStringLiteral("name"), payload},
    };
  }
  if (kind == Kind::PutSymbol) {
    return {
        {QStringLiteral("op"), QStringLiteral("symbol.put")},
        {QStringLiteral("v"), 1},
        {QStringLiteral("symbol_id"), entity_id},
        {QStringLiteral("after_symbol_id"), after_id.isEmpty()
                                                ? QJsonValue(QJsonValue::Null)
                                                : QJsonValue(after_id)},
        {QStringLiteral("expected_version"), expected_version},
        {QStringLiteral("xml"), payload},
    };
  }
  return {
      {QStringLiteral("op"), QStringLiteral("object.put")},
      {QStringLiteral("v"), 1},
      {QStringLiteral("object_id"), entity_id},
      {QStringLiteral("part_id"), parent_id},
      {QStringLiteral("after_object_id"), after_id.isEmpty()
                                              ? QJsonValue(QJsonValue::Null)
                                              : QJsonValue(after_id)},
      {QStringLiteral("expected_version"), expected_version},
      {QStringLiteral("xml"), payload},
  };
}

bool MapHubEditTransaction::isValid(QString *error) const {
  if (!canonicalUuid(client_instance_id) || !canonicalUuid(transaction_id) ||
      client_sequence < 1 || expected_stream_sequence < 0 ||
      !validHash(expected_stream_hash) || operations.isEmpty() ||
      operations.size() > maximum_operations ||
      (!expected_workspace_revision_id.isEmpty() &&
       !canonicalUuid(expected_workspace_revision_id)) ||
      (!expected_project_revision_id.isEmpty() &&
       !canonicalUuid(expected_project_revision_id))) {
    if (error)
      *error =
          QStringLiteral("The Map Hub edit transaction envelope is invalid.");
    return false;
  }
  qsizetype aggregate_xml = 0;
  QSet<QString> touched;
  for (const auto &operation : operations) {
    if (!canonicalUuid(operation.entity_id) || operation.expected_version < 0 ||
        touched.contains(operation.entity_id)) {
      if (error)
        *error = QStringLiteral(
            "A Map Hub transaction contains an invalid or duplicate entity.");
      return false;
    }
    touched.insert(operation.entity_id);
    if (operation.kind == MapHubEditOperation::Kind::PutObject) {
      if (!canonicalUuid(operation.parent_id) ||
          (!operation.after_id.isEmpty() &&
           !canonicalUuid(operation.after_id)) ||
          operation.after_id == operation.entity_id ||
          operation.payload.toUtf8().size() > maximum_fragment_bytes ||
          !validXmlFragment(operation.payload, QStringLiteral("object"),
                            operation.entity_id)) {
        if (error)
          *error = QStringLiteral(
              "A Map Hub object fragment or ordering anchor is invalid.");
        return false;
      }
      aggregate_xml += operation.payload.toUtf8().size();
    } else if (operation.kind == MapHubEditOperation::Kind::PutPart) {
      if ((!operation.after_id.isEmpty() &&
           !canonicalUuid(operation.after_id)) ||
          operation.after_id == operation.entity_id ||
          operation.payload.isEmpty() || operation.payload.size() > 512) {
        if (error)
          *error = QStringLiteral(
              "A Map Hub part name or ordering anchor is invalid.");
        return false;
      }
    } else if (operation.kind == MapHubEditOperation::Kind::PutSymbol) {
      if ((!operation.after_id.isEmpty() &&
           !canonicalUuid(operation.after_id)) ||
          operation.after_id == operation.entity_id ||
          operation.payload.toUtf8().size() > maximum_fragment_bytes ||
          !validXmlFragment(operation.payload, QStringLiteral("symbol"),
                            operation.entity_id)) {
        if (error)
          *error = QStringLiteral(
              "A Map Hub symbol fragment or ordering anchor is invalid.");
        return false;
      }
      aggregate_xml += operation.payload.toUtf8().size();
    } else if (operation.expected_version == 0) {
      if (error)
        *error = QStringLiteral("An entity must exist in the synchronized "
                                "projection before it can be deleted.");
      return false;
    }
  }
  if (aggregate_xml > maximum_aggregate_fragment_bytes) {
    if (error)
      *error = QStringLiteral("The Map Hub transaction contains too much XML.");
    return false;
  }
  return true;
}

QJsonObject MapHubEditTransaction::toJson() const {
  QJsonArray operation_array;
  for (const auto &operation : operations)
    operation_array.append(operation.toJson());
  return {
      {QStringLiteral("protocol"), QString::fromLatin1(protocol)},
      {QStringLiteral("client_instance_id"), client_instance_id},
      {QStringLiteral("client_sequence"), client_sequence},
      {QStringLiteral("transaction_id"), transaction_id},
      {QStringLiteral("expected"),
       QJsonObject{
           {QStringLiteral("stream_sequence"), expected_stream_sequence},
           {QStringLiteral("stream_hash"), expected_stream_hash},
           {QStringLiteral("workspace_revision_id"),
            expected_workspace_revision_id.isEmpty()
                ? QJsonValue(QJsonValue::Null)
                : QJsonValue(expected_workspace_revision_id)},
           {QStringLiteral("project_revision_id"),
            expected_project_revision_id.isEmpty()
                ? QJsonValue(QJsonValue::Null)
                : QJsonValue(expected_project_revision_id)},
       }},
      {QStringLiteral("operations"), operation_array},
  };
}

MapHubEditTransaction MapHubEditTransaction::fromJson(const QJsonObject &object,
                                                      QString *error) {
  MapHubEditTransaction transaction;
  if (object.value(QStringLiteral("protocol")).toString() !=
          QLatin1String(protocol) ||
      !object.value(QStringLiteral("expected")).isObject() ||
      !object.value(QStringLiteral("operations")).isArray()) {
    if (error)
      *error = QStringLiteral("The Map Hub transaction shape is invalid.");
    return {};
  }
  transaction.client_instance_id =
      object.value(QStringLiteral("client_instance_id")).toString();
  transaction.client_sequence =
      object.value(QStringLiteral("client_sequence")).toInteger(-1);
  transaction.transaction_id =
      object.value(QStringLiteral("transaction_id")).toString();
  const auto expected = object.value(QStringLiteral("expected")).toObject();
  transaction.expected_stream_sequence =
      expected.value(QStringLiteral("stream_sequence")).toInteger(-1);
  transaction.expected_stream_hash =
      expected.value(QStringLiteral("stream_hash")).toString();
  if (!expected.value(QStringLiteral("workspace_revision_id")).isNull())
    transaction.expected_workspace_revision_id =
        expected.value(QStringLiteral("workspace_revision_id")).toString();
  if (!expected.value(QStringLiteral("project_revision_id")).isNull())
    transaction.expected_project_revision_id =
        expected.value(QStringLiteral("project_revision_id")).toString();

  for (const auto &value :
       object.value(QStringLiteral("operations")).toArray()) {
    if (!value.isObject()) {
      if (error)
        *error =
            QStringLiteral("A Map Hub transaction operation is not an object.");
      return {};
    }
    const auto operation_object = value.toObject();
    if (operation_object.value(QStringLiteral("v")).toInteger(-1) != 1) {
      if (error)
        *error = QStringLiteral("A Map Hub operation version is unsupported.");
      return {};
    }
    MapHubEditOperation operation;
    const auto name = operation_object.value(QStringLiteral("op")).toString();
    if (name == QLatin1String("object.put")) {
      operation.kind = MapHubEditOperation::Kind::PutObject;
      operation.parent_id =
          operation_object.value(QStringLiteral("part_id")).toString();
      if (!operation_object.value(QStringLiteral("after_object_id")).isNull())
        operation.after_id =
            operation_object.value(QStringLiteral("after_object_id"))
                .toString();
      operation.payload =
          operation_object.value(QStringLiteral("xml")).toString();
      operation.entity_id =
          operation_object.value(QStringLiteral("object_id")).toString();
    } else if (name == QLatin1String("object.delete")) {
      operation.kind = MapHubEditOperation::Kind::DeleteObject;
      operation.entity_id =
          operation_object.value(QStringLiteral("object_id")).toString();
    } else if (name == QLatin1String("part.put")) {
      operation.kind = MapHubEditOperation::Kind::PutPart;
      operation.entity_id =
          operation_object.value(QStringLiteral("part_id")).toString();
      if (!operation_object.value(QStringLiteral("after_part_id")).isNull())
        operation.after_id =
            operation_object.value(QStringLiteral("after_part_id")).toString();
      operation.payload =
          operation_object.value(QStringLiteral("name")).toString();
    } else if (name == QLatin1String("part.delete")) {
      operation.kind = MapHubEditOperation::Kind::DeletePart;
      operation.entity_id =
          operation_object.value(QStringLiteral("part_id")).toString();
    } else if (name == QLatin1String("symbol.put")) {
      operation.kind = MapHubEditOperation::Kind::PutSymbol;
      operation.entity_id =
          operation_object.value(QStringLiteral("symbol_id")).toString();
      if (!operation_object.value(QStringLiteral("after_symbol_id")).isNull())
        operation.after_id =
            operation_object.value(QStringLiteral("after_symbol_id"))
                .toString();
      operation.payload =
          operation_object.value(QStringLiteral("xml")).toString();
    } else if (name == QLatin1String("symbol.delete")) {
      operation.kind = MapHubEditOperation::Kind::DeleteSymbol;
      operation.entity_id =
          operation_object.value(QStringLiteral("symbol_id")).toString();
    } else {
      if (error)
        *error = QStringLiteral("A Map Hub operation is unsupported.");
      return {};
    }
    operation.expected_version =
        operation_object.value(QStringLiteral("expected_version"))
            .toInteger(-1);
    transaction.operations.push_back(std::move(operation));
  }
  if (!transaction.isValid(error))
    return {};
  return transaction;
}

QByteArray MapHubEditTransaction::canonicalBytes(QString *error) const {
  if (!isValid(error))
    return {};
  auto bytes = OomJson::canonical(toJson(), error);
  if (bytes.size() > maximum_transaction_bytes) {
    if (error)
      *error = QStringLiteral("The Map Hub transaction exceeds 1 MiB.");
    return {};
  }
  return bytes;
}

QString MapHubEditTransaction::payloadSha256(QString *error) const {
  const auto bytes = canonicalBytes(error);
  if (bytes.isEmpty())
    return {};
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

MapHubEditTransaction MapHubEditTransaction::fromUndoStep(
    const Map &map, const UndoStep &step,
    const QHash<QString, qint64> &entity_versions,
    const QString &client_instance_id, qint64 client_sequence,
    qint64 expected_stream_sequence, const QString &expected_stream_hash,
    const QString &expected_workspace_revision_id,
    const QString &expected_project_revision_id, QString *error) {
  MapHubEditTransaction transaction;
  transaction.client_instance_id = client_instance_id;
  transaction.client_sequence = client_sequence;
  transaction.transaction_id =
      QUuid::createUuid().toString(QUuid::WithoutBraces);
  transaction.expected_stream_sequence = expected_stream_sequence;
  transaction.expected_stream_hash = expected_stream_hash;
  transaction.expected_workspace_revision_id = expected_workspace_revision_id;
  transaction.expected_project_revision_id = expected_project_revision_id;

  std::vector<UndoStep::EntityChange> changes;
  step.collectEntityChanges(changes);
  QStringList affected_ids;
  for (const auto &change : changes) {
    if (!affected_ids.contains(change.object_id))
      affected_ids.append(change.object_id);
  }

  struct PendingPut {
    LocatedObject location;
    QString id;
  };
  QVector<PendingPut> puts;
  QStringList deletes;
  for (const auto &id : affected_ids) {
    auto location = locate(map, id);
    if (location.object)
      puts.push_back({location, id});
    else
      deletes.append(id);
  }
  std::sort(puts.begin(), puts.end(), [](const auto &left, const auto &right) {
    return std::tie(left.location.part_index, left.location.object_index) <
           std::tie(right.location.part_index, right.location.object_index);
  });
  for (const auto &put : puts) {
    const auto *part = put.location.part;
    QString after;
    if (put.location.object_index > 0)
      after = part->getObject(put.location.object_index - 1)->persistentId();
    transaction.operations.push_back({
        MapHubEditOperation::Kind::PutObject,
        put.id,
        part->persistentId(),
        after,
        entity_versions.value(put.id, 0),
        serializeObject(*put.location.object),
    });
  }
  for (const auto &id : deletes) {
    transaction.operations.push_back({
        MapHubEditOperation::Kind::DeleteObject,
        id,
        {},
        {},
        entity_versions.value(id, 0),
        {},
    });
  }

  if (!transaction.isValid(error))
    return {};
  return transaction;
}

QString MapHubEditTransaction::objectFragment(const Object &object) {
  return serializeObject(object);
}

QString MapHubEditTransaction::symbolFragment(const Symbol &symbol,
                                              const Map &map) {
  QByteArray bytes;
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::WriteOnly))
    return {};
  QXmlStreamWriter xml(&buffer);
  xml.setAutoFormatting(false);
  QScopedValueRollback<int> format_version(XMLFileFormat::active_version,
                                           XMLFileFormat::current_version);
  symbol.save(xml, map);
  buffer.close();
  return QString::fromUtf8(bytes);
}

bool MapHubCommittedTransaction::isValid(QString *error) const {
  if (!transaction.isValid(error) || stream_sequence < 1 ||
      !validHash(payload_sha256) || !validHash(stream_hash) ||
      transaction.payloadSha256(error) != payload_sha256) {
    if (error && error->isEmpty())
      *error = QStringLiteral("A committed Map Hub transaction is invalid.");
    return false;
  }
  return true;
}

MapHubCommittedTransaction
MapHubCommittedTransaction::fromJson(const QJsonObject &object,
                                     QString *error) {
  MapHubCommittedTransaction committed;
  committed.transaction = MapHubEditTransaction::fromJson(object, error);
  committed.stream_sequence =
      object.value(QStringLiteral("stream_sequence")).toInteger(-1);
  committed.payload_sha256 =
      object.value(QStringLiteral("payload_sha256")).toString();
  committed.stream_hash =
      object.value(QStringLiteral("stream_hash")).toString();
  committed.committed_at =
      object.value(QStringLiteral("committed_at")).toString();
  if (!committed.isValid(error))
    return {};
  return committed;
}

} // namespace OpenOrienteering
