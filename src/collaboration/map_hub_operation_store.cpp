/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_operation_store.h"

#include <sqlite3.h>

#include <tuple>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QStandardPaths>
#include <QTimeZone>
#include <QUuid>

#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "core/symbols/symbol.h"

namespace OpenOrienteering {

namespace {

constexpr auto zero_hash =
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr int schema_version = 1;

QString sqliteError(sqlite3 *database, const QString &fallback) {
  if (!database)
    return fallback;
  const auto *message = sqlite3_errmsg(database);
  return message ? QString::fromUtf8(message) : fallback;
}

bool exec(sqlite3 *database, const char *sql, QString *error) {
  char *message = nullptr;
  const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
  if (result == SQLITE_OK)
    return true;
  if (error)
    *error = message ? QString::fromUtf8(message)
                     : sqliteError(database, QStringLiteral("SQLite failed."));
  sqlite3_free(message);
  return false;
}

class Statement {
public:
  Statement(sqlite3 *database, const char *sql, QString *error) {
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) !=
            SQLITE_OK &&
        error)
      *error = sqliteError(database, QStringLiteral("SQLite prepare failed."));
  }
  ~Statement() { sqlite3_finalize(statement); }
  explicit operator bool() const { return statement; }
  sqlite3_stmt *get() const { return statement; }

private:
  sqlite3_stmt *statement = nullptr;
};

void bindText(sqlite3_stmt *statement, int index, const QString &value) {
  const auto utf8 = value.toUtf8();
  sqlite3_bind_text(statement, index, utf8.constData(), utf8.size(),
                    SQLITE_TRANSIENT);
}

QString columnText(sqlite3_stmt *statement, int index) {
  const auto *text = sqlite3_column_text(statement, index);
  return text ? QString::fromUtf8(reinterpret_cast<const char *>(text))
              : QString{};
}

bool setMeta(sqlite3 *database, const QString &key, const QString &value,
             QString *error) {
  Statement statement(database,
                      "INSERT INTO meta(key,value) VALUES(?1,?2) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                      error);
  if (!statement)
    return false;
  bindText(statement.get(), 1, key);
  bindText(statement.get(), 2, value);
  if (sqlite3_step(statement.get()) == SQLITE_DONE)
    return true;
  if (error)
    *error = sqliteError(database, QStringLiteral("SQLite write failed."));
  return false;
}

QString meta(sqlite3 *database, const QString &key, QString *error) {
  Statement statement(database, "SELECT value FROM meta WHERE key=?1", error);
  if (!statement)
    return {};
  bindText(statement.get(), 1, key);
  const auto result = sqlite3_step(statement.get());
  if (result == SQLITE_ROW)
    return columnText(statement.get(), 0);
  if (result != SQLITE_DONE && error)
    *error = sqliteError(database, QStringLiteral("SQLite read failed."));
  return {};
}

class Transaction {
public:
  Transaction(sqlite3 *database, QString *error) : database(database) {
    active = exec(database, "BEGIN IMMEDIATE", error);
  }
  ~Transaction() {
    if (active)
      sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr);
  }
  explicit operator bool() const { return active; }
  bool commit(QString *error) {
    if (!active || !exec(database, "COMMIT", error))
      return false;
    active = false;
    return true;
  }

private:
  sqlite3 *database = nullptr;
  bool active = false;
};

} // namespace

MapHubOperationStore::MapHubOperationStore() = default;
MapHubOperationStore::~MapHubOperationStore() = default;

void MapHubOperationStore::DatabaseCloser::operator()(sqlite3 *value) const {
  if (value)
    sqlite3_close_v2(value);
}

QString MapHubOperationStore::databasePath(const QString &workspace_id) {
  const QUuid uuid(workspace_id);
  if (uuid.isNull())
    return {};
  auto root = qEnvironmentVariable("MAPPER_MAP_HUB_SYNC_ROOT");
  if (root.isEmpty())
    root =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("map-hub-sync"));
  return QDir(root).filePath(QStringLiteral("%1/operations.sqlite3")
                                 .arg(uuid.toString(QUuid::WithoutBraces)));
}

bool MapHubOperationStore::open(const QString &workspace_id, QString *error) {
  close();
  const auto path = databasePath(workspace_id);
  if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
    if (error)
      *error = QStringLiteral(
          "Mapper could not create the Map Hub operation store.");
    return false;
  }
  sqlite3 *opened = nullptr;
  if (sqlite3_open_v2(path.toUtf8().constData(), &opened,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    if (error)
      *error = sqliteError(opened, QStringLiteral("Cannot open SQLite."));
    sqlite3_close_v2(opened);
    return false;
  }
  database.reset(opened);
  sqlite3_busy_timeout(database.get(), 5000);
  int existing_version = 0;
  {
    Statement version(database.get(), "PRAGMA user_version", error);
    if (!version || sqlite3_step(version.get()) != SQLITE_ROW) {
      close();
      return false;
    }
    existing_version = sqlite3_column_int(version.get(), 0);
  }
  if (existing_version > schema_version) {
    if (error)
      *error = QStringLiteral(
          "This Map Hub operation store was created by a newer Mapper.");
    close();
    return false;
  }
  const char *schema =
      "PRAGMA journal_mode=WAL;"
      "PRAGMA synchronous=FULL;"
      "PRAGMA foreign_keys=ON;"
      "PRAGMA temp_store=MEMORY;"
      "CREATE TABLE IF NOT EXISTS meta("
      " key TEXT PRIMARY KEY,value TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS entities("
      " id TEXT PRIMARY KEY,kind TEXT NOT NULL,version INTEGER NOT NULL,"
      " tombstone INTEGER NOT NULL DEFAULT 0,parent_id TEXT,after_id TEXT);"
      "CREATE TABLE IF NOT EXISTS outbox("
      " client_sequence INTEGER PRIMARY KEY,transaction_id TEXT NOT NULL "
      "UNIQUE,"
      " canonical_json BLOB NOT NULL,payload_sha256 TEXT NOT NULL,"
      " predicted_stream_sequence INTEGER NOT NULL,"
      " predicted_stream_hash TEXT NOT NULL,created_ms INTEGER NOT NULL,"
      " attempt_count INTEGER NOT NULL DEFAULT 0,next_attempt_ms INTEGER,"
      " last_error_code TEXT,last_error_message TEXT);"
      "PRAGMA user_version=1;";
  if (!exec(database.get(), schema, error)) {
    close();
    return false;
  }
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  QFile::setPermissions(path + QStringLiteral("-wal"),
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  QFile::setPermissions(path + QStringLiteral("-shm"),
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  if (meta(database.get(), QStringLiteral("client_instance_id"), error)
          .isEmpty()) {
    Transaction transaction(database.get(), error);
    if (!transaction)
      return false;
    const auto client_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!setMeta(database.get(), QStringLiteral("client_instance_id"),
                 client_id, error) ||
        !setMeta(database.get(), QStringLiteral("acknowledged_client_sequence"),
                 QStringLiteral("0"), error) ||
        !setMeta(database.get(), QStringLiteral("published_stream_sequence"),
                 QStringLiteral("0"), error) ||
        !setMeta(database.get(), QStringLiteral("published_stream_hash"),
                 QString::fromLatin1(zero_hash), error) ||
        !setMeta(database.get(), QStringLiteral("optimistic_stream_sequence"),
                 QStringLiteral("0"), error) ||
        !setMeta(database.get(), QStringLiteral("optimistic_stream_hash"),
                 QString::fromLatin1(zero_hash), error) ||
        !transaction.commit(error))
      return false;
  }
  return true;
}

void MapHubOperationStore::close() { database.reset(); }
bool MapHubOperationStore::isOpen() const noexcept { return bool(database); }

MapHubOperationStore::State MapHubOperationStore::state(QString *error) const {
  if (!database)
    return {};
  State result;
  result.client_instance_id =
      meta(database.get(), QStringLiteral("client_instance_id"), error);
  result.acknowledged_client_sequence =
      meta(database.get(), QStringLiteral("acknowledged_client_sequence"),
           error)
          .toLongLong();
  result.published_stream_sequence =
      meta(database.get(), QStringLiteral("published_stream_sequence"), error)
          .toLongLong();
  result.published_stream_hash =
      meta(database.get(), QStringLiteral("published_stream_hash"), error);
  result.optimistic_stream_sequence =
      meta(database.get(), QStringLiteral("optimistic_stream_sequence"), error)
          .toLongLong();
  result.optimistic_stream_hash =
      meta(database.get(), QStringLiteral("optimistic_stream_hash"), error);
  return result;
}

QHash<QString, qint64>
MapHubOperationStore::entityVersions(QString *error) const {
  QHash<QString, qint64> result;
  if (!database)
    return result;
  Statement statement(database.get(), "SELECT id,version FROM entities", error);
  if (!statement)
    return result;
  int step;
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW)
    result.insert(columnText(statement.get(), 0),
                  sqlite3_column_int64(statement.get(), 1));
  if (step != SQLITE_DONE && error)
    *error = sqliteError(database.get(), QStringLiteral("SQLite read failed."));
  Statement pending(
      database.get(),
      "SELECT canonical_json FROM outbox ORDER BY client_sequence", error);
  if (!pending)
    return {};
  while ((step = sqlite3_step(pending.get())) == SQLITE_ROW) {
    const auto *blob = sqlite3_column_blob(pending.get(), 0);
    const auto bytes = sqlite3_column_bytes(pending.get(), 0);
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(static_cast<const char *>(blob), bytes), &parse_error);
    QString transaction_error;
    const auto transaction = document.isObject()
                                 ? MapHubEditTransaction::fromJson(
                                       document.object(), &transaction_error)
                                 : MapHubEditTransaction{};
    if (parse_error.error != QJsonParseError::NoError ||
        !transaction.isValid(&transaction_error)) {
      if (error)
        *error = transaction_error.isEmpty()
                     ? QStringLiteral("The durable Map Hub outbox is corrupt.")
                     : transaction_error;
      return {};
    }
    for (const auto &operation : transaction.operations)
      result[operation.object_id] = operation.expected_version + 1;
  }
  if (step != SQLITE_DONE && error)
    *error = sqliteError(database.get(), QStringLiteral("SQLite read failed."));
  return result;
}

MapHubEntityIndex MapHubOperationStore::entityIndex(QString *error) const {
  MapHubEntityIndex index;
  if (!database)
    return index;
  const auto current = state(error);
  index.stream_sequence = current.published_stream_sequence;
  index.stream_hash = current.published_stream_hash;
  Statement statement(
      database.get(),
      "SELECT kind,id,version,tombstone,parent_id,after_id FROM entities",
      error);
  if (!statement)
    return {};
  int step;
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    index.entities.push_back({
        columnText(statement.get(), 0),
        columnText(statement.get(), 1),
        sqlite3_column_int64(statement.get(), 2),
        sqlite3_column_int(statement.get(), 3) != 0,
        columnText(statement.get(), 4),
        columnText(statement.get(), 5),
    });
  }
  if (step != SQLITE_DONE) {
    if (error)
      *error =
          sqliteError(database.get(), QStringLiteral("SQLite read failed."));
    return {};
  }
  auto kind_order = [](const QString &kind) {
    if (kind == QLatin1String("part"))
      return 0;
    if (kind == QLatin1String("symbol"))
      return 1;
    return 2;
  };
  std::sort(index.entities.begin(), index.entities.end(),
            [&kind_order](const auto &left, const auto &right) {
              return std::tuple(kind_order(left.kind), left.id) <
                     std::tuple(kind_order(right.kind), right.id);
            });
  if (!index.isValid(error))
    return {};
  return index;
}

bool MapHubOperationStore::seedInitialProjection(const Map &map,
                                                 QString *error) {
  if (!database)
    return false;
  Statement count(database.get(), "SELECT count(*) FROM entities", error);
  if (!count || sqlite3_step(count.get()) != SQLITE_ROW)
    return false;
  if (sqlite3_column_int64(count.get(), 0) != 0)
    return true;

  Transaction transaction(database.get(), error);
  if (!transaction)
    return false;
  Statement insert(
      database.get(),
      "INSERT INTO entities(id,kind,version,tombstone,parent_id,after_id)"
      " VALUES(?1,?2,1,0,?3,?4)",
      error);
  if (!insert)
    return false;
  auto add = [&](const QString &id, const QString &kind, const QString &parent,
                 const QString &after) {
    sqlite3_reset(insert.get());
    sqlite3_clear_bindings(insert.get());
    bindText(insert.get(), 1, id);
    bindText(insert.get(), 2, kind);
    if (parent.isEmpty())
      sqlite3_bind_null(insert.get(), 3);
    else
      bindText(insert.get(), 3, parent);
    if (after.isEmpty())
      sqlite3_bind_null(insert.get(), 4);
    else
      bindText(insert.get(), 4, after);
    return sqlite3_step(insert.get()) == SQLITE_DONE;
  };
  QString previous;
  for (int i = 0; i < map.getNumSymbols(); ++i) {
    const auto *symbol = map.getSymbol(i);
    if (!add(symbol->persistentId(), QStringLiteral("symbol"), {}, previous))
      return false;
    previous = symbol->persistentId();
  }
  previous.clear();
  for (int p = 0; p < map.getNumParts(); ++p) {
    const auto *part = map.getPart(p);
    if (!add(part->persistentId(), QStringLiteral("part"), {}, previous))
      return false;
    previous = part->persistentId();
    QString previous_object;
    for (int o = 0; o < part->getNumObjects(); ++o) {
      const auto *object = part->getObject(o);
      if (!add(object->persistentId(), QStringLiteral("object"),
               part->persistentId(), previous_object))
        return false;
      previous_object = object->persistentId();
    }
  }
  return transaction.commit(error);
}

bool MapHubOperationStore::replaceProjection(const MapHubEntityIndex &index,
                                             QString *error) {
  if (!database || !index.isValid(error) || pendingCount(error) != 0)
    return false;
  Transaction transaction(database.get(), error);
  if (!transaction || !exec(database.get(), "DELETE FROM entities", error))
    return false;
  Statement insert(
      database.get(),
      "INSERT INTO entities(id,kind,version,tombstone,parent_id,after_id)"
      " VALUES(?1,?2,?3,?4,?5,?6)",
      error);
  if (!insert)
    return false;
  for (const auto &entity : index.entities) {
    sqlite3_reset(insert.get());
    sqlite3_clear_bindings(insert.get());
    bindText(insert.get(), 1, entity.id);
    bindText(insert.get(), 2, entity.kind);
    sqlite3_bind_int64(insert.get(), 3, entity.version);
    sqlite3_bind_int(insert.get(), 4, entity.tombstone);
    if (entity.parent_id.isEmpty())
      sqlite3_bind_null(insert.get(), 5);
    else
      bindText(insert.get(), 5, entity.parent_id);
    if (entity.after_id.isEmpty())
      sqlite3_bind_null(insert.get(), 6);
    else
      bindText(insert.get(), 6, entity.after_id);
    if (sqlite3_step(insert.get()) != SQLITE_DONE)
      return false;
  }
  if (!setMeta(database.get(), QStringLiteral("published_stream_sequence"),
               QString::number(index.stream_sequence), error) ||
      !setMeta(database.get(), QStringLiteral("published_stream_hash"),
               index.stream_hash, error) ||
      !setMeta(database.get(), QStringLiteral("optimistic_stream_sequence"),
               QString::number(index.stream_sequence), error) ||
      !setMeta(database.get(), QStringLiteral("optimistic_stream_hash"),
               index.stream_hash, error))
    return false;
  return transaction.commit(error);
}

QString MapHubOperationStore::chainHash(const QString &previous_hash,
                                        const QString &payload_sha256) {
  const auto previous = QByteArray::fromHex(previous_hash.toLatin1());
  const auto payload = QByteArray::fromHex(payload_sha256.toLatin1());
  if (previous.size() != 32 || payload.size() != 32)
    return {};
  auto material = previous;
  material.append(payload);
  return QString::fromLatin1(
      QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}

bool MapHubOperationStore::enqueue(
    const MapHubEditTransaction &transaction_value, QString *error) {
  if (!database)
    return false;
  const auto current = state(error);
  if (transaction_value.client_instance_id != current.client_instance_id ||
      transaction_value.client_sequence !=
          current.acknowledged_client_sequence + pendingCount(error) + 1 ||
      transaction_value.expected_stream_sequence !=
          current.optimistic_stream_sequence ||
      transaction_value.expected_stream_hash !=
          current.optimistic_stream_hash) {
    if (error)
      *error = QStringLiteral(
          "The Map Hub operation store advanced before this edit was queued.");
    return false;
  }
  const auto versions = entityVersions(error);
  for (const auto &operation : transaction_value.operations) {
    if (versions.value(operation.object_id, 0) != operation.expected_version) {
      if (error)
        *error = QStringLiteral(
            "A local Map Hub entity version changed while queueing.");
      return false;
    }
  }
  const auto canonical = transaction_value.canonicalBytes(error);
  const auto digest = transaction_value.payloadSha256(error);
  const auto predicted_sequence =
      transaction_value.expected_stream_sequence + 1;
  const auto predicted_hash =
      chainHash(transaction_value.expected_stream_hash, digest);
  if (canonical.isEmpty() || predicted_hash.isEmpty())
    return false;

  Transaction transaction(database.get(), error);
  if (!transaction)
    return false;
  Statement insert(
      database.get(),
      "INSERT INTO outbox(client_sequence,transaction_id,canonical_json,"
      "payload_sha256,predicted_stream_sequence,predicted_stream_hash,"
      "created_ms) VALUES(?1,?2,?3,?4,?5,?6,?7)",
      error);
  if (!insert)
    return false;
  sqlite3_bind_int64(insert.get(), 1, transaction_value.client_sequence);
  bindText(insert.get(), 2, transaction_value.transaction_id);
  sqlite3_bind_blob(insert.get(), 3, canonical.constData(), canonical.size(),
                    SQLITE_TRANSIENT);
  bindText(insert.get(), 4, digest);
  sqlite3_bind_int64(insert.get(), 5, predicted_sequence);
  bindText(insert.get(), 6, predicted_hash);
  sqlite3_bind_int64(insert.get(), 7, QDateTime::currentMSecsSinceEpoch());
  if (sqlite3_step(insert.get()) != SQLITE_DONE) {
    if (error)
      *error =
          sqliteError(database.get(), QStringLiteral("Queue write failed."));
    return false;
  }

  if (!setMeta(database.get(), QStringLiteral("optimistic_stream_sequence"),
               QString::number(predicted_sequence), error) ||
      !setMeta(database.get(), QStringLiteral("optimistic_stream_hash"),
               predicted_hash, error))
    return false;
  return transaction.commit(error);
}

MapHubOperationStore::Pending
MapHubOperationStore::nextPending(QString *error) const {
  Pending result;
  if (!database)
    return result;
  Statement statement(
      database.get(),
      "SELECT client_sequence,transaction_id,canonical_json,payload_sha256,"
      "predicted_stream_sequence,predicted_stream_hash,attempt_count,"
      "next_attempt_ms,last_error_code,last_error_message"
      " FROM outbox ORDER BY client_sequence LIMIT 1",
      error);
  if (!statement || sqlite3_step(statement.get()) != SQLITE_ROW)
    return result;
  result.client_sequence = sqlite3_column_int64(statement.get(), 0);
  result.transaction_id = columnText(statement.get(), 1);
  const auto *blob = sqlite3_column_blob(statement.get(), 2);
  const auto bytes = sqlite3_column_bytes(statement.get(), 2);
  result.canonical_json = QByteArray(static_cast<const char *>(blob), bytes);
  result.payload_sha256 = columnText(statement.get(), 3);
  result.predicted_stream_sequence = sqlite3_column_int64(statement.get(), 4);
  result.predicted_stream_hash = columnText(statement.get(), 5);
  result.attempt_count = sqlite3_column_int(statement.get(), 6);
  if (sqlite3_column_type(statement.get(), 7) != SQLITE_NULL)
    result.next_attempt_at = QDateTime::fromMSecsSinceEpoch(
        sqlite3_column_int64(statement.get(), 7), QTimeZone::UTC);
  result.last_error_code = columnText(statement.get(), 8);
  result.last_error_message = columnText(statement.get(), 9);
  return result;
}

QVector<MapHubEditTransaction>
MapHubOperationStore::pendingTransactions(QString *error) const {
  QVector<MapHubEditTransaction> result;
  if (!database)
    return result;
  Statement statement(
      database.get(),
      "SELECT canonical_json FROM outbox ORDER BY client_sequence", error);
  if (!statement)
    return result;
  int step;
  while ((step = sqlite3_step(statement.get())) == SQLITE_ROW) {
    const auto *blob = sqlite3_column_blob(statement.get(), 0);
    const auto bytes = sqlite3_column_bytes(statement.get(), 0);
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(static_cast<const char *>(blob), bytes), &parse_error);
    QString transaction_error;
    auto transaction = document.isObject()
                           ? MapHubEditTransaction::fromJson(document.object(),
                                                             &transaction_error)
                           : MapHubEditTransaction{};
    if (parse_error.error != QJsonParseError::NoError ||
        !transaction.isValid(&transaction_error)) {
      if (error)
        *error = transaction_error.isEmpty()
                     ? QStringLiteral("The durable Map Hub outbox is corrupt.")
                     : transaction_error;
      return {};
    }
    result.push_back(std::move(transaction));
  }
  if (step != SQLITE_DONE) {
    if (error)
      *error =
          sqliteError(database.get(), QStringLiteral("SQLite read failed."));
    return {};
  }
  return result;
}

bool MapHubOperationStore::acknowledge(qint64 client_sequence,
                                       qint64 stream_sequence,
                                       const QString &stream_hash,
                                       QString *error) {
  const auto pending = nextPending(error);
  if (!pending.isValid() || pending.client_sequence != client_sequence ||
      pending.predicted_stream_sequence != stream_sequence ||
      pending.predicted_stream_hash != stream_hash) {
    if (error)
      *error = QStringLiteral(
          "Map Hub acknowledged a different operation stream position.");
    return false;
  }
  Transaction transaction(database.get(), error);
  if (!transaction)
    return false;
  QJsonParseError parse_error;
  const auto document =
      QJsonDocument::fromJson(pending.canonical_json, &parse_error);
  QString transaction_error;
  const auto accepted =
      document.isObject() ? MapHubEditTransaction::fromJson(document.object(),
                                                            &transaction_error)
                          : MapHubEditTransaction{};
  if (parse_error.error != QJsonParseError::NoError ||
      !accepted.isValid(&transaction_error)) {
    if (error)
      *error = transaction_error.isEmpty()
                   ? QStringLiteral("The durable Map Hub outbox is corrupt.")
                   : transaction_error;
    return false;
  }
  for (const auto &operation : accepted.operations) {
    Statement update(
        database.get(),
        "INSERT INTO entities(id,kind,version,tombstone,parent_id,after_id)"
        " VALUES(?1,'object',?2,?3,?4,?5)"
        " ON CONFLICT(id) DO UPDATE SET version=excluded.version,"
        " tombstone=excluded.tombstone,parent_id=excluded.parent_id,"
        " after_id=excluded.after_id",
        error);
    if (!update)
      return false;
    bindText(update.get(), 1, operation.object_id);
    sqlite3_bind_int64(update.get(), 2, operation.expected_version + 1);
    sqlite3_bind_int(update.get(), 3,
                     operation.kind == MapHubEditOperation::Kind::DeleteObject);
    if (operation.part_id.isEmpty())
      sqlite3_bind_null(update.get(), 4);
    else
      bindText(update.get(), 4, operation.part_id);
    if (operation.after_object_id.isEmpty())
      sqlite3_bind_null(update.get(), 5);
    else
      bindText(update.get(), 5, operation.after_object_id);
    if (sqlite3_step(update.get()) != SQLITE_DONE)
      return false;
  }
  Statement remove(database.get(),
                   "DELETE FROM outbox WHERE client_sequence=?1", error);
  if (!remove)
    return false;
  sqlite3_bind_int64(remove.get(), 1, client_sequence);
  if (sqlite3_step(remove.get()) != SQLITE_DONE ||
      sqlite3_changes(database.get()) != 1)
    return false;
  if (!setMeta(database.get(), QStringLiteral("acknowledged_client_sequence"),
               QString::number(client_sequence), error) ||
      !setMeta(database.get(), QStringLiteral("published_stream_sequence"),
               QString::number(stream_sequence), error) ||
      !setMeta(database.get(), QStringLiteral("published_stream_hash"),
               stream_hash, error))
    return false;
  return transaction.commit(error);
}

bool MapHubOperationStore::recordFailure(qint64 client_sequence,
                                         const QString &code,
                                         const QString &message,
                                         const QDateTime &next_attempt_at,
                                         QString *error) {
  Statement statement(
      database.get(),
      "UPDATE outbox SET attempt_count=attempt_count+1,next_attempt_ms=?2,"
      "last_error_code=?3,last_error_message=?4 WHERE client_sequence=?1",
      error);
  if (!statement)
    return false;
  sqlite3_bind_int64(statement.get(), 1, client_sequence);
  sqlite3_bind_int64(statement.get(), 2, next_attempt_at.toMSecsSinceEpoch());
  bindText(statement.get(), 3, code);
  bindText(statement.get(), 4, message);
  return sqlite3_step(statement.get()) == SQLITE_DONE &&
         sqlite3_changes(database.get()) == 1;
}

int MapHubOperationStore::pendingCount(QString *error) const {
  if (!database)
    return 0;
  Statement statement(database.get(), "SELECT count(*) FROM outbox", error);
  if (!statement || sqlite3_step(statement.get()) != SQLITE_ROW)
    return 0;
  return sqlite3_column_int(statement.get(), 0);
}

bool MapHubOperationStore::rebaseOnto(
    const QVector<MapHubCommittedTransaction> &transactions, QString *error) {
  if (!database || transactions.isEmpty())
    return bool(database);
  const auto before = state(error);
  qint64 expected_sequence = before.published_stream_sequence;
  QString expected_hash = before.published_stream_hash;

  QVector<MapHubEditTransaction> pending_transactions;
  Statement pending_statement(
      database.get(),
      "SELECT canonical_json FROM outbox ORDER BY client_sequence", error);
  if (!pending_statement)
    return false;
  int step;
  while ((step = sqlite3_step(pending_statement.get())) == SQLITE_ROW) {
    const auto *blob = sqlite3_column_blob(pending_statement.get(), 0);
    const auto bytes = sqlite3_column_bytes(pending_statement.get(), 0);
    QJsonParseError parse_error;
    const auto document = QJsonDocument::fromJson(
        QByteArray(static_cast<const char *>(blob), bytes), &parse_error);
    QString parse_message;
    const auto pending =
        document.isObject()
            ? MapHubEditTransaction::fromJson(document.object(), &parse_message)
            : MapHubEditTransaction{};
    if (parse_error.error != QJsonParseError::NoError ||
        !pending.isValid(&parse_message)) {
      if (error)
        *error = parse_message.isEmpty()
                     ? QStringLiteral("The durable Map Hub outbox is corrupt.")
                     : parse_message;
      return false;
    }
    pending_transactions.push_back(pending);
  }
  if (step != SQLITE_DONE) {
    if (error)
      *error =
          sqliteError(database.get(), QStringLiteral("SQLite read failed."));
    return false;
  }

  QSet<QString> locally_touched;
  QSet<QString> local_dependencies;
  for (const auto &pending : pending_transactions) {
    for (const auto &operation : pending.operations) {
      locally_touched.insert(operation.object_id);
      if (!operation.after_object_id.isEmpty())
        local_dependencies.insert(operation.after_object_id);
      if (!operation.part_id.isEmpty())
        local_dependencies.insert(operation.part_id);
    }
  }
  for (const auto &committed : transactions) {
    QString validation_error;
    if (!committed.isValid(&validation_error) ||
        committed.stream_sequence != expected_sequence + 1 ||
        committed.transaction.expected_stream_sequence != expected_sequence ||
        committed.transaction.expected_stream_hash != expected_hash ||
        chainHash(expected_hash, committed.payload_sha256) !=
            committed.stream_hash) {
      if (error)
        *error = validation_error.isEmpty()
                     ? QStringLiteral(
                           "Map Hub returned a discontinuous operation chain.")
                     : validation_error;
      return false;
    }
    for (const auto &operation : committed.transaction.operations) {
      if (locally_touched.contains(operation.object_id) ||
          (operation.kind == MapHubEditOperation::Kind::DeleteObject &&
           local_dependencies.contains(operation.object_id))) {
        if (error)
          *error = QStringLiteral(
              "An upstream edit touches the same map object as local work.");
        return false;
      }
    }
    expected_sequence = committed.stream_sequence;
    expected_hash = committed.stream_hash;
  }

  QHash<QString, qint64> projection;
  Statement entities(database.get(), "SELECT id,version FROM entities", error);
  if (!entities)
    return false;
  while ((step = sqlite3_step(entities.get())) == SQLITE_ROW)
    projection.insert(columnText(entities.get(), 0),
                      sqlite3_column_int64(entities.get(), 1));
  if (step != SQLITE_DONE)
    return false;

  Transaction database_transaction(database.get(), error);
  if (!database_transaction)
    return false;
  for (const auto &committed : transactions) {
    for (const auto &operation : committed.transaction.operations) {
      if (projection.value(operation.object_id, 0) !=
          operation.expected_version) {
        if (error)
          *error = QStringLiteral(
              "The local Map Hub projection does not match the server.");
        return false;
      }
      Statement update(
          database.get(),
          "INSERT INTO entities(id,kind,version,tombstone,parent_id,after_id)"
          " VALUES(?1,'object',?2,?3,?4,?5)"
          " ON CONFLICT(id) DO UPDATE SET version=excluded.version,"
          " tombstone=excluded.tombstone,parent_id=excluded.parent_id,"
          " after_id=excluded.after_id",
          error);
      if (!update)
        return false;
      bindText(update.get(), 1, operation.object_id);
      sqlite3_bind_int64(update.get(), 2, operation.expected_version + 1);
      sqlite3_bind_int(update.get(), 3,
                       operation.kind ==
                           MapHubEditOperation::Kind::DeleteObject);
      if (operation.part_id.isEmpty())
        sqlite3_bind_null(update.get(), 4);
      else
        bindText(update.get(), 4, operation.part_id);
      if (operation.after_object_id.isEmpty())
        sqlite3_bind_null(update.get(), 5);
      else
        bindText(update.get(), 5, operation.after_object_id);
      if (sqlite3_step(update.get()) != SQLITE_DONE)
        return false;
      projection[operation.object_id] = operation.expected_version + 1;
    }
  }
  if (!setMeta(database.get(), QStringLiteral("published_stream_sequence"),
               QString::number(expected_sequence), error) ||
      !setMeta(database.get(), QStringLiteral("published_stream_hash"),
               expected_hash, error))
    return false;

  auto optimistic_sequence = expected_sequence;
  auto optimistic_hash = expected_hash;
  for (auto &pending : pending_transactions) {
    pending.expected_stream_sequence = optimistic_sequence;
    pending.expected_stream_hash = optimistic_hash;
    pending.transaction_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    for (auto &operation : pending.operations) {
      operation.expected_version = projection.value(operation.object_id, 0);
      projection[operation.object_id] = operation.expected_version + 1;
    }
    const auto canonical = pending.canonicalBytes(error);
    const auto digest = pending.payloadSha256(error);
    const auto predicted_hash = chainHash(optimistic_hash, digest);
    if (canonical.isEmpty() || predicted_hash.isEmpty())
      return false;
    ++optimistic_sequence;
    Statement rewrite(
        database.get(),
        "UPDATE outbox SET transaction_id=?2,canonical_json=?3,"
        "payload_sha256=?4,predicted_stream_sequence=?5,"
        "predicted_stream_hash=?6,attempt_count=0,next_attempt_ms=NULL,"
        "last_error_code=NULL,last_error_message=NULL"
        " WHERE client_sequence=?1",
        error);
    if (!rewrite)
      return false;
    sqlite3_bind_int64(rewrite.get(), 1, pending.client_sequence);
    bindText(rewrite.get(), 2, pending.transaction_id);
    sqlite3_bind_blob(rewrite.get(), 3, canonical.constData(), canonical.size(),
                      SQLITE_TRANSIENT);
    bindText(rewrite.get(), 4, digest);
    sqlite3_bind_int64(rewrite.get(), 5, optimistic_sequence);
    bindText(rewrite.get(), 6, predicted_hash);
    if (sqlite3_step(rewrite.get()) != SQLITE_DONE ||
        sqlite3_changes(database.get()) != 1)
      return false;
    optimistic_hash = predicted_hash;
  }
  if (!setMeta(database.get(), QStringLiteral("optimistic_stream_sequence"),
               QString::number(optimistic_sequence), error) ||
      !setMeta(database.get(), QStringLiteral("optimistic_stream_hash"),
               optimistic_hash, error))
    return false;
  return database_transaction.commit(error);
}

} // namespace OpenOrienteering
