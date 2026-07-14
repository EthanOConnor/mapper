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
