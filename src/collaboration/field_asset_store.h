/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_FIELD_ASSET_STORE_H
#define OPENORIENTEERING_FIELD_ASSET_STORE_H

#include <QString>

namespace OpenOrienteering {

/**
 * Content-addressed local store for synchronized field assets (GPX tracklogs).
 *
 * Layout: `<root>/<sha256>/<original_name>` with a `staging/` scratch area
 * which is promoted by an atomic directory rename, following the
 * ImageryCatalogStore discipline in a much simpler form. Concurrent writers
 * are serialized by a QLockFile at the store root.
 */
class FieldAssetStore {
public:
  /** Uses AppDataLocation/field-assets when root is empty. */
  explicit FieldAssetStore(QString root = {});

  QString rootPath() const { return root; }

  static bool isValidSha256(const QString &sha256);

  /** Template resource identity for a synced tracklog:
   * "maphub:field-asset:<sha256>". */
  static QString resourceIdentityFor(const QString &sha256);
  /** Inverse of resourceIdentityFor(); empty when the identity is not a
   * valid field-asset identity. */
  static QString sha256FromResourceIdentity(const QString &identity);

  bool has(const QString &sha256) const;
  /** Absolute path of the stored asset file, or empty when absent. */
  QString pathFor(const QString &sha256) const;

  /** Scratch file path for downloading the given asset. The parent
   * directory is created. Empty on failure. */
  QString stagingPathFor(const QString &sha256) const;
  /** Moves a completely downloaded staging file into its content address.
   * An already present asset wins; the staging file is always consumed. */
  bool promote(const QString &sha256, const QString &original_name,
               const QString &staged_file, QString *error = nullptr);

  /** File name safe for storage and content-disposition headers. */
  static QString sanitizedName(const QString &original_name);

private:
  QString root;
};

} // namespace OpenOrienteering

#endif
