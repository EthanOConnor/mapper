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

#include "document_path.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace OpenOrienteering::DocumentPath {

bool isContentUri(QStringView path)
{
	if (!path.startsWith(u"content://", Qt::CaseInsensitive))
		return false;
	return QUrl{path.toString()}.scheme().compare(QLatin1String("content"), Qt::CaseInsensitive) == 0;
}

QString fromUrl(const QUrl& url)
{
	if (!url.isValid() || url.isEmpty())
		return {};
	if (url.isLocalFile())
		return url.toLocalFile();
	return url.toString(QUrl::FullyEncoded);
}

QUrl toUrl(QStringView path)
{
	if (path.isEmpty())
		return {};
	if (isContentUri(path))
		return QUrl{path.toString()};
	if (QDir::isAbsolutePath(path.toString()))
		return QUrl::fromLocalFile(path.toString());
	const auto candidate = QUrl{path.toString()};
	if (candidate.isLocalFile() || !candidate.scheme().isEmpty())
		return candidate;
	return QUrl::fromLocalFile(path.toString());
}

QString canonical(QStringView path)
{
	if (path.isEmpty())
		return {};

	if (isContentUri(path))
		return QUrl{path.toString()}.toString(QUrl::FullyEncoded);

	const auto url = QUrl{path.toString()};
	if (url.isLocalFile())
		return canonical(url.toLocalFile());
	if (!QDir::isAbsolutePath(path.toString()) && !url.scheme().isEmpty())
		return path.toString();

	const QFileInfo info{path.toString()};
	const auto canonical_path = info.canonicalFilePath();
	return canonical_path.isEmpty() ? info.absoluteFilePath() : canonical_path;
}

QString displayName(QStringView path)
{
	if (isContentUri(path))
	{
		auto name = QUrl{path.toString()}.fileName(QUrl::FullyDecoded);
		const auto separator = name.lastIndexOf(QLatin1Char('/'));
		if (separator >= 0)
			name = name.sliced(separator + 1);
		if (!name.isEmpty())
			return name;
	}
	return QFileInfo{path.toString()}.fileName();
}

QString suffix(QStringView path)
{
	return QFileInfo{displayName(path)}.suffix();
}

QString autosavePath(QStringView path)
{
	if (!isContentUri(path))
		return path.toString() + QLatin1String(".autosave");

	auto root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if (root.isEmpty())
		root = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	QDir directory{root};
	directory.mkpath(QLatin1String("autosave"));
	const auto identity = canonical(path).toUtf8();
	const auto digest = QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
	return directory.filePath(QLatin1String("autosave/")
	                          + QString::fromLatin1(digest)
	                          + QLatin1String(".omap.autosave"));
}

}  // namespace OpenOrienteering::DocumentPath
