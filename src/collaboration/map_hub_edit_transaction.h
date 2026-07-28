/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_EDIT_TRANSACTION_H
#define OPENORIENTEERING_MAP_HUB_EDIT_TRANSACTION_H

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace OpenOrienteering {

class Map;
class UndoStep;

struct MapHubEditOperation {
  enum class Kind { PutObject, DeleteObject };

  Kind kind = Kind::PutObject;
  QString object_id;
  QString part_id;
  QString after_object_id;
  qint64 expected_version = 0;
  QString xml;

  QJsonObject toJson() const;
};

struct MapHubEditTransaction {
  static constexpr auto protocol = "oom-map-ops/1";

  QString client_instance_id;
  qint64 client_sequence = 0;
  QString transaction_id;
  qint64 expected_stream_sequence = 0;
  QString expected_stream_hash;
  QString expected_workspace_revision_id;
  QString expected_project_revision_id;
  QVector<MapHubEditOperation> operations;

  bool isValid(QString *error = nullptr) const;
  QJsonObject toJson() const;
  static MapHubEditTransaction fromJson(const QJsonObject &object,
                                        QString *error = nullptr);
  QByteArray canonicalBytes(QString *error = nullptr) const;
  QString payloadSha256(QString *error = nullptr) const;

  static MapHubEditTransaction fromUndoStep(
      const Map &map, const UndoStep &step,
      const QHash<QString, qint64> &entity_versions,
      const QString &client_instance_id, qint64 client_sequence,
      qint64 expected_stream_sequence, const QString &expected_stream_hash,
      const QString &expected_workspace_revision_id,
      const QString &expected_project_revision_id, QString *error = nullptr);
};

struct MapHubCommittedTransaction {
  MapHubEditTransaction transaction;
  qint64 stream_sequence = 0;
  QString payload_sha256;
  QString stream_hash;
  QString committed_at;

  bool isValid(QString *error = nullptr) const;
  static MapHubCommittedTransaction fromJson(const QJsonObject &object,
                                             QString *error = nullptr);
};

} // namespace OpenOrienteering

#endif
