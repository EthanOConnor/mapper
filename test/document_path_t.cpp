/*
 *    Copyright 2026 The OpenOrienteering developers
 *
 *    This file is part of OpenOrienteering.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 */

#include "document_path_t.h"

#include <QtTest>
#include <QDir>
#include <QFileInfo>
#include <QLatin1String>
#include <QUrl>

#include "core/document_path.h"

using namespace OpenOrienteering;

void DocumentPathTest::localPathRoundTrip()
{
	const auto path = QDir::temp().filePath(QLatin1String("mapper document/map.omap"));
	const auto url = QUrl::fromLocalFile(path);

	QCOMPARE(DocumentPath::fromUrl(url), path);
	QCOMPARE(DocumentPath::toUrl(path), url);
	QCOMPARE(DocumentPath::canonical(path), QFileInfo{path}.absoluteFilePath());
	QCOMPARE(DocumentPath::displayName(path), QLatin1String("map.omap"));
	QCOMPARE(DocumentPath::suffix(path), QLatin1String("omap"));

	const auto unresolved_relative = QStringLiteral("missing/templates/map.omap");
	QCOMPARE(DocumentPath::canonical(unresolved_relative), unresolved_relative);
}

void DocumentPathTest::contentUriRoundTrip()
{
	const auto uri = QStringLiteral(
		"content://com.android.providers.downloads.documents/"
		"document/primary%3ADownload%2FClub%20Map.omap");

	QVERIFY(DocumentPath::isContentUri(uri));
	QCOMPARE(DocumentPath::fromUrl(DocumentPath::toUrl(uri)), uri);
	QCOMPARE(DocumentPath::canonical(uri), uri);
	QCOMPARE(DocumentPath::displayName(uri), QStringLiteral("Club Map.omap"));
	QCOMPARE(DocumentPath::suffix(uri), QLatin1String("omap"));
}

void DocumentPathTest::legacyAndroidPathMigration()
{
	const auto primary_path = QStringLiteral(
		"/storage/emulated/0/OOMapper/Club Maps/Forest.omap");
	const auto primary_tree = QStringLiteral(
		"content://com.android.externalstorage.documents/tree/primary%3AOOMapper");
	QVERIFY(DocumentPath::isLegacyAndroidSharedPath(primary_path));
	QCOMPARE(DocumentPath::legacyAndroidFolderUrl(primary_path).toString(QUrl::FullyEncoded),
	         QStringLiteral("content://com.android.externalstorage.documents/"
	                        "document/primary%3AOOMapper"));
	QCOMPARE(DocumentPath::legacyAndroidDocumentUri(primary_tree, primary_path),
	         QStringLiteral("content://com.android.externalstorage.documents/"
	                        "tree/primary%3AOOMapper/document/"
	                        "primary%3AOOMapper%2FClub%20Maps%2FForest.omap"));
	QCOMPARE(DocumentPath::legacyAndroidDocumentUri(
		QStringLiteral("content://com.android.externalstorage.documents/tree/primary%3Aoomapper"),
		primary_path),
		QStringLiteral("content://com.android.externalstorage.documents/"
		               "tree/primary%3Aoomapper/document/"
		               "primary%3Aoomapper%2FClub%20Maps%2FForest.omap"));

	const auto card_path = QStringLiteral("/storage/1234-ABCD/OOMapper/Event/map.omap");
	const auto card_tree = QStringLiteral(
		"content://com.android.externalstorage.documents/tree/1234-ABCD%3AOOMapper/"
		"document/1234-ABCD%3AOOMapper");
	QVERIFY(DocumentPath::isLegacyAndroidSharedPath(card_path));
	QCOMPARE(DocumentPath::legacyAndroidDocumentUri(card_tree, card_path),
	         QStringLiteral("content://com.android.externalstorage.documents/"
	                        "tree/1234-ABCD%3AOOMapper/document/"
	                        "1234-ABCD%3AOOMapper%2FEvent%2Fmap.omap"));

	QVERIFY(!DocumentPath::isLegacyAndroidSharedPath(
		QStringLiteral("/storage/emulated/0/Download/map.omap")));
	QVERIFY(DocumentPath::legacyAndroidDocumentUri(
		QStringLiteral("content://com.android.externalstorage.documents/tree/primary%3ADownload"),
		primary_path).isEmpty());
	QVERIFY(DocumentPath::legacyAndroidDocumentUri(primary_tree, card_path).isEmpty());
	QVERIFY(!DocumentPath::isLegacyAndroidSharedPath(
		QStringLiteral("/storage/emulated/0/OOMapper/../Download/map.omap")));
}

void DocumentPathTest::autosaveLocation()
{
	const auto local = QDir::temp().filePath(QLatin1String("map.omap"));
	QCOMPARE(DocumentPath::autosavePath(local), local + QLatin1String(".autosave"));

	const auto first_uri = QStringLiteral("content://documents/document/primary%3AMaps%2Ffirst.omap");
	const auto second_uri = QStringLiteral("content://documents/document/primary%3AMaps%2Fsecond.omap");
	const auto first_path = DocumentPath::autosavePath(first_uri);
	QCOMPARE(DocumentPath::autosavePath(first_uri), first_path);
	QVERIFY(first_path != DocumentPath::autosavePath(second_uri));
	QVERIFY(QFileInfo{first_path}.isAbsolute());
	QVERIFY(first_path.endsWith(QLatin1String(".omap.autosave")));
}

QTEST_APPLESS_MAIN(DocumentPathTest)
