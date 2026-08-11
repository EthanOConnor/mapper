/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_OPERATION_STORE_H
#define OPENORIENTEERING_MAP_HUB_OPERATION_STORE_H

#include <memory>

#include <QDateTime>
#include <QHash>
#include <QString>

#include "collaboration/map_hub_edit_transaction.h"
#include "collaboration/map_hub_entity_index.h"

struct sqlite3;

namespace OpenOrienteering {

class Map;

class MapHubOperationStore {
public:
  struct State {
    QString client_instance_id;
    qint64 acknowledged_client_sequence = 0;
    qint64 published_stream_sequence = 0;
    QString published_stream_hash;
    qint64 optimistic_stream_sequence = 0;
    QString optimistic_stream_hash;
  };

  struct Pending {
    qint64 client_sequence = 0;
    QString transaction_id;
    QByteArray canonical_json;
    QString payload_sha256;
    qint64 predicted_stream_sequence = 0;
    QString predicted_stream_hash;
    int attempt_count = 0;
    QDateTime next_attempt_at;
    QString last_error_code;
    QString last_error_message;
    MapHubEditTransaction transaction;

    bool isValid() const {
      return client_sequence > 0 && !transaction_id.isEmpty() &&
             !canonical_json.isEmpty() && payload_sha256.size() == 64 &&
             predicted_stream_sequence > 0 &&
             predicted_stream_hash.size() == 64 && transaction.isValid() &&
             transaction.client_sequence == client_sequence &&
             transaction.transaction_id == transaction_id &&
             transaction.payloadSha256() == payload_sha256 &&
             transaction.expected_stream_sequence + 1 ==
                 predicted_stream_sequence &&
             MapHubOperationStore::chainHash(transaction.expected_stream_hash,
                                             payload_sha256) ==
                 predicted_stream_hash;
    }
  };

  MapHubOperationStore();
  ~MapHubOperationStore();

  MapHubOperationStore(const MapHubOperationStore &) = delete;
  MapHubOperationStore &operator=(const MapHubOperationStore &) = delete;

  bool open(const QString &workspace_id, QString *error = nullptr);
  /** Opens the durable store and binds a newly created store to the requested
   * client identity. An existing store with a different identity is rejected. */
  bool open(const QString &workspace_id,
            const QString &requested_client_instance_id,
            QString *error = nullptr);
  void close();
  bool isOpen() const noexcept;

  State state(QString *error = nullptr) const;
  QHash<QString, qint64> entityVersions(QString *error = nullptr) const;
  MapHubEntityIndex entityIndex(QString *error = nullptr) const;
  bool seedInitialProjection(const Map &map, QString *error = nullptr);
  bool replaceProjection(const MapHubEntityIndex &index,
                         QString *error = nullptr);
  bool enqueue(const MapHubEditTransaction &transaction,
               QString *error = nullptr);
  Pending nextPending(QString *error = nullptr) const;
  QVector<MapHubEditTransaction>
  pendingTransactions(QString *error = nullptr) const;
  QVector<MapHubCommittedTransaction>
  unappliedTransactions(QString *error = nullptr) const;
  bool markTransactionsApplied(qint64 through_sequence,
                               QString *error = nullptr);
  bool acknowledge(qint64 client_sequence, qint64 stream_sequence,
                   const QString &stream_hash, QString *error = nullptr);
  bool recordFailure(qint64 client_sequence, const QString &code,
                     const QString &message, const QDateTime &next_attempt_at,
                     QString *error = nullptr);
  int pendingCount(QString *error = nullptr) const;
  bool rebaseOnto(const QVector<MapHubCommittedTransaction> &transactions,
                  QString *error = nullptr);
  bool rebasePendingOntoSnapshot(const MapHubEntityIndex &index,
                                 const QString &expected_workspace_revision_id,
                                 const QString &expected_project_revision_id,
                                 QString *error = nullptr);

  static QString databasePath(const QString &workspace_id);
  static QString chainHash(const QString &previous_hash,
                           const QString &payload_sha256);

private:
  struct DatabaseCloser {
    void operator()(sqlite3 *database) const;
  };
  std::unique_ptr<sqlite3, DatabaseCloser> database;
};

} // namespace OpenOrienteering

#endif
