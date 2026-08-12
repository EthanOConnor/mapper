/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "field_asset_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLatin1String>
#include <QLockFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>

namespace OpenOrienteering {

namespace {

const QLatin1String identity_prefix("maphub:field-asset:");

bool fail(QString *error, const QString &message) {
  if (error)
    *error = message;
  return false;
}

} // namespace

FieldAssetStore::FieldAssetStore(QString root)
    : root(root.isEmpty()
               ? QDir(QStandardPaths::writableLocation(
                          QStandardPaths::AppDataLocation))
                     .filePath(QStringLiteral("field-assets"))
               : std::move(root)) {}

bool FieldAssetStore::isValidSha256(const QString &sha256) {
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return pattern.match(sha256).hasMatch();
}

QString FieldAssetStore::resourceIdentityFor(const QString &sha256) {
  if (!isValidSha256(sha256))
    return {};
  return identity_prefix + sha256;
}

QString FieldAssetStore::sha256FromResourceIdentity(const QString &identity) {
  if (!identity.startsWith(identity_prefix))
    return {};
  auto sha256 = identity.mid(identity_prefix.size());
  return isValidSha256(sha256) ? sha256 : QString{};
}

bool FieldAssetStore::has(const QString &sha256) const {
  return !pathFor(sha256).isEmpty();
}

QString FieldAssetStore::pathFor(const QString &sha256) const {
  if (!isValidSha256(sha256))
    return {};
  const QDir asset_dir(QDir(root).filePath(sha256));
  if (!asset_dir.exists())
    return {};
  const auto entries =
      asset_dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
  for (const auto &entry : entries) {
    if (!entry.isSymLink())
      return entry.absoluteFilePath();
  }
  return {};
}

QString FieldAssetStore::stagingPathFor(const QString &sha256) const {
  if (!isValidSha256(sha256))
    return {};
  const auto staging_dir = QDir(root).filePath(QStringLiteral("staging"));
  if (!QDir().mkpath(staging_dir))
    return {};
  return QDir(staging_dir).filePath(sha256 + QLatin1String(".download"));
}

bool FieldAssetStore::promote(const QString &sha256,
                              const QString &original_name,
                              const QString &staged_file, QString *error) {
  if (!isValidSha256(sha256)) {
    QFile::remove(staged_file);
    return fail(error, QStringLiteral("Invalid field-asset content address."));
  }
  if (!QFileInfo::exists(staged_file))
    return fail(error, QStringLiteral("The staged field asset is missing."));
  if (!QDir().mkpath(root)) {
    QFile::remove(staged_file);
    return fail(error,
                QStringLiteral("Could not create the field-asset store."));
  }

  QLockFile lock(QDir(root).filePath(QStringLiteral(".lock")));
  lock.setStaleLockTime(60 * 1000);
  if (!lock.tryLock(10 * 1000)) {
    QFile::remove(staged_file);
    return fail(error, QStringLiteral(
                           "Another process is updating the field-asset "
                           "store."));
  }

  if (has(sha256)) {
    // Content-addressed: an existing asset is identical by construction.
    QFile::remove(staged_file);
    return true;
  }

  const auto promote_dir =
      QDir(root).filePath(QStringLiteral("staging/") + sha256
                          + QLatin1String(".new"));
  QDir(promote_dir).removeRecursively();
  if (!QDir().mkpath(promote_dir)) {
    QFile::remove(staged_file);
    return fail(error, QStringLiteral(
                           "Could not create the field-asset staging "
                           "directory."));
  }
  const auto target_file =
      QDir(promote_dir).filePath(sanitizedName(original_name));
  if (!QFile::rename(staged_file, target_file)) {
    QDir(promote_dir).removeRecursively();
    QFile::remove(staged_file);
    return fail(error,
                QStringLiteral("Could not stage the field asset for "
                               "promotion."));
  }
  const auto final_dir = QDir(root).filePath(sha256);
  if (!QDir().rename(promote_dir, final_dir)) {
    QDir(promote_dir).removeRecursively();
    return fail(error,
                QStringLiteral("Could not promote the field asset into the "
                               "store."));
  }
  return true;
}

QString FieldAssetStore::sanitizedName(const QString &original_name) {
  auto name = QFileInfo(original_name).fileName();
  static const QRegularExpression forbidden(
      QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
  name.remove(forbidden);
  name = name.trimmed();
  if (name.isEmpty() || name == QLatin1String(".")
      || name == QLatin1String("..") || name.startsWith(QLatin1Char('.')))
    name = QStringLiteral("tracklog.gpx");
  if (!name.endsWith(QLatin1String(".gpx"), Qt::CaseInsensitive))
    name += QLatin1String(".gpx");
  return name;
}

} // namespace OpenOrienteering
