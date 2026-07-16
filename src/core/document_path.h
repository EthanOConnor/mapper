/*
 *    Copyright 2026 The OpenOrienteering developers
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 */

#ifndef OPENORIENTEERING_DOCUMENT_PATH_H
#define OPENORIENTEERING_DOCUMENT_PATH_H

#include <QString>
#include <QStringView>
#include <QUrl>

namespace OpenOrienteering::DocumentPath {

/** Returns true for an Android Storage Access Framework document URI. */
bool isContentUri(QStringView path);

/** Returns true for a pre-scoped-storage path in a public OOMapper folder. */
bool isLegacyAndroidSharedPath(QStringView path);

/** Returns the system-picker URI for the legacy path's OOMapper folder. */
QUrl legacyAndroidFolderUrl(QStringView path);

/** Maps a legacy public path into a document URI below an authorized tree. */
QString legacyAndroidDocumentUri(QStringView tree_uri, QStringView path);

/** Resolves a legacy public path through the previously authorized tree. */
QString resolveForAccess(QStringView path);

/** Returns the persisted Android document-tree URI, if any. */
QString androidDocumentTreeUri();

/** Stores the Android document-tree URI after access has been verified. */
void setAndroidDocumentTreeUri(QStringView tree_uri);

/** Persists the Android system picker's read/write grant for a document tree. */
bool persistAndroidDocumentTreeAccess(QStringView tree_uri);

/** Returns true if Android can open the document for reading and writing. */
bool canReadWriteAndroidDocument(QStringView document_uri);

/** Converts a file or document URL to the identifier accepted by QFile. */
QString fromUrl(const QUrl& url);

/** Converts a QFile document identifier to a URL for QFileDialog. */
QUrl toUrl(QStringView path);

/** Returns a stable identity for an existing local file or document URI. */
QString canonical(QStringView path);

/** Returns the user-facing name of a local file or document URI. */
QString displayName(QStringView path);

/** Returns the filename suffix of a local file or document URI. */
QString suffix(QStringView path);

/** Returns a local autosave path for a local file or document URI. */
QString autosavePath(QStringView path);

}  // namespace OpenOrienteering::DocumentPath

#endif
