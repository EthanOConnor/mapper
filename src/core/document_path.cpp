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
#include <QSettings>
#include <QStandardPaths>

#ifdef Q_OS_ANDROID
#  include <QCoreApplication>
#  include <QJniObject>
#endif

namespace {

constexpr auto android_document_tree_key = "androidDocumentTreeUri";
constexpr auto external_storage_authority = "com.android.externalstorage.documents";

struct LegacyAndroidPath
{
	QString volume;
	QString relative_path;
};

LegacyAndroidPath splitLegacyAndroidPath(QStringView path)
{
	struct Root
	{
		QStringView path;
		QStringView volume;
	};
	static constexpr Root primary_roots[] = {
		{ u"/storage/emulated/0/OOMapper/", u"primary" },
		{ u"/storage/self/primary/OOMapper/", u"primary" },
		{ u"/sdcard/OOMapper/", u"primary" },
	};

	LegacyAndroidPath result;
	for (const auto& root : primary_roots)
	{
		if (path.startsWith(root.path, Qt::CaseInsensitive))
		{
			result.volume = root.volume.toString();
			result.relative_path = path.sliced(root.path.size()).toString();
			break;
		}
	}

	if (result.relative_path.isEmpty() && path.startsWith(u"/storage/"))
	{
		const auto volume_start = QStringView{u"/storage/"}.size();
		const auto volume_end = path.indexOf(u'/', volume_start);
		if (volume_end > volume_start)
		{
			const auto volume = path.sliced(volume_start, volume_end - volume_start);
			static constexpr auto folder = QStringView{u"/OOMapper/"};
			if (volume.compare(u"emulated", Qt::CaseInsensitive) != 0
			    && volume.compare(u"self", Qt::CaseInsensitive) != 0
			    && path.sliced(volume_end).startsWith(folder, Qt::CaseInsensitive))
			{
				result.volume = volume.toString();
				result.relative_path = path.sliced(volume_end + folder.size()).toString();
			}
		}
	}

	if (result.relative_path.isEmpty()
	    || result.relative_path.startsWith(u'/')
	    || result.relative_path.split(u'/').contains(QLatin1String("..")))
	{
		return {};
	}
	return result;
}

}  // namespace

namespace OpenOrienteering::DocumentPath {

bool isContentUri(QStringView path)
{
	if (!path.startsWith(u"content://", Qt::CaseInsensitive))
		return false;
	return QUrl{path.toString()}.scheme().compare(QLatin1String("content"), Qt::CaseInsensitive) == 0;
}

bool isLegacyAndroidSharedPath(QStringView path)
{
	return !splitLegacyAndroidPath(path).relative_path.isEmpty();
}

QUrl legacyAndroidFolderUrl(QStringView path)
{
	const auto legacy = splitLegacyAndroidPath(path);
	if (legacy.relative_path.isEmpty())
		return {};
	const auto document_id = legacy.volume + QLatin1String(":OOMapper");
	return QUrl{QLatin1String("content://") + QLatin1String(external_storage_authority)
	            + QLatin1String("/document/")
	            + QString::fromLatin1(QUrl::toPercentEncoding(document_id))};
}

QString legacyAndroidDocumentUri(QStringView tree_uri, QStringView path)
{
	const auto legacy = splitLegacyAndroidPath(path);
	if (legacy.relative_path.isEmpty())
		return {};

	const QUrl tree_url{tree_uri.toString()};
	if (tree_url.scheme().compare(QLatin1String("content"), Qt::CaseInsensitive) != 0
	    || tree_url.authority().compare(QLatin1String(external_storage_authority), Qt::CaseInsensitive) != 0)
	{
		return {};
	}

	const auto encoded_path = tree_url.path(QUrl::FullyEncoded);
	static constexpr auto marker = QStringView{u"/tree/"};
	if (!encoded_path.startsWith(marker))
		return {};
	const auto tree_id_end = encoded_path.indexOf(u'/', marker.size());
	const auto encoded_tree_id = QStringView{encoded_path}.sliced(
		marker.size(), tree_id_end < 0 ? encoded_path.size() - marker.size()
		                               : tree_id_end - marker.size());
	const auto tree_id = QUrl::fromPercentEncoding(encoded_tree_id.toLatin1());
	const auto separator = tree_id.indexOf(QLatin1Char(':'));
	if (separator <= 0
	    || tree_id.sliced(separator + 1).compare(QLatin1String("OOMapper"), Qt::CaseInsensitive) != 0
	    || tree_id.first(separator).compare(legacy.volume, Qt::CaseInsensitive) != 0)
	{
		return {};
	}

	const auto document_id = tree_id + QLatin1Char('/') + legacy.relative_path;
	return QLatin1String("content://") + tree_url.authority()
	       + QLatin1String("/tree/") + encoded_tree_id
	       + QLatin1String("/document/")
	       + QString::fromLatin1(QUrl::toPercentEncoding(document_id));
}

QString androidDocumentTreeUri()
{
	return QSettings{}.value(QLatin1String(android_document_tree_key)).toString();
}

void setAndroidDocumentTreeUri(QStringView tree_uri)
{
	QSettings settings;
	if (tree_uri.isEmpty())
		settings.remove(QLatin1String(android_document_tree_key));
	else
		settings.setValue(QLatin1String(android_document_tree_key), tree_uri.toString());
}

QString resolveForAccess(QStringView path)
{
#ifdef Q_OS_ANDROID
	if (isLegacyAndroidSharedPath(path))
	{
		const auto resolved = legacyAndroidDocumentUri(androidDocumentTreeUri(), path);
		if (!resolved.isEmpty())
			return resolved;
	}
#endif
	return path.toString();
}

bool persistAndroidDocumentTreeAccess(QStringView tree_uri)
{
#ifdef Q_OS_ANDROID
	const auto activity = QNativeInterface::QAndroidApplication::context();
	return activity.isValid()
	       && activity.callMethod<jboolean>("persistDocumentTreeUri",
	                                        tree_uri.toString());
#else
	Q_UNUSED(tree_uri)
	return false;
#endif
}

bool canReadWriteAndroidDocument(QStringView document_uri)
{
#ifdef Q_OS_ANDROID
	const auto activity = QNativeInterface::QAndroidApplication::context();
	return activity.isValid()
	       && activity.callMethod<jboolean>("canReadWriteDocument",
	                                        document_uri.toString());
#else
	Q_UNUSED(document_uri)
	return false;
#endif
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
	if (!canonical_path.isEmpty())
		return canonical_path;
	return info.isAbsolute() ? info.absoluteFilePath() : path.toString();
}

QString displayName(QStringView path)
{
	if (isContentUri(path))
	{
#ifdef Q_OS_ANDROID
		const auto provider_name = QFileInfo{path.toString()}.fileName();
		if (!provider_name.isEmpty())
			return provider_name;
#endif
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
