/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "manual_imagery_source_t.h"

#include <cmath>
#include <utility>

#include <QUrlQuery>
#include <QtTest>

#include "imagery/manual_imagery_source.h"

using namespace OpenOrienteering;

void ManualImagerySourceTest::acceptsAliasesAndBuildsDirectDefaults()
{
	imagery::ManualTiledSourceSettings settings;
	settings.max_zoom = 3;
	auto const result = imagery::ManualImagerySource::classify(
		QStringLiteral(
			"https://tiles.example.test/aerial/${z}/${x}/${y}.png"
		),
		settings
	);
	QVERIFY2(result.isDirect(), qPrintable(result.detail));
	QCOMPARE(
		result.outcome, imagery::ManualImageryOutcome::Direct
	);
	QCOMPARE(
		result.input_kind,
		imagery::ManualImageryInputKind::TiledUrlTemplate
	);
	QCOMPARE(
		result.normalized_template,
		QStringLiteral(
			"https://tiles.example.test/aerial/{z}/{x}/{y}.png"
		)
	);
	QCOMPARE(result.suggested_name, QStringLiteral("tiles.example.test"));
	QVERIFY(result.source);
	auto const& source = *result.source;
	QVERIFY(source.metadata.id.startsWith(QStringLiteral("manual-")));
	auto const canonical = imagery::ManualImagerySource::classify(
		QStringLiteral(
			"https://tiles.example.test/aerial/{z}/{x}/{y}.png"
		),
		settings
	);
	QVERIFY(canonical.isDirect());
	QCOMPARE(canonical.source->metadata.id, source.metadata.id);
	QCOMPARE(source.metadata.name, QStringLiteral("tiles.example.test"));
	QCOMPARE(source.row_scheme, imagery::TileRowScheme::Xyz);
	QCOMPARE(source.min_zoom, 0);
	QCOMPARE(source.max_zoom, 3);
	QCOMPARE(
		source.request.empty_http_status_codes,
		QVector<int>({ 204, 404 })
	);
	QCOMPARE(source.tile_matrix_set.crs, QStringLiteral("EPSG:3857"));
	QCOMPARE(source.tile_matrix_set.matrices.size(), 4);
	QCOMPARE(
		source.tile_matrix_set.matrices.first().tile_size,
		QSize(256, 256)
	);
	QString error;
	QVERIFY2(source.validate(&error), qPrintable(error));
	QCOMPARE(
		source.tileUrl(0, 2, 1, 3, &error),
		QUrl(QStringLiteral(
			"https://tiles.example.test/aerial/2/1/3.png"
		))
	);
}

void ManualImagerySourceTest::appliesTmsAnd512PixelAdvancedSettings()
{
	imagery::ManualTiledSourceSettings settings;
	settings.id = QStringLiteral("manual.custom");
	settings.name = QStringLiteral("Custom imagery");
	settings.scheme = imagery::TileRowScheme::Tms;
	settings.min_zoom = 1;
	settings.max_zoom = 3;
	settings.tile_size = 512;
	settings.media_type = QStringLiteral("image/jpeg");
	settings.referer =
		QUrl(QStringLiteral("https://maps.example.test/"));
	settings.empty_http_status_codes = { 404 };
	settings.attribution_text = QStringLiteral("Example Imagery");
	settings.attribution_url =
		QUrl(QStringLiteral("https://example.test/attribution"));

	auto const result = imagery::ManualImagerySource::classify(
		QStringLiteral(
			"https://tiles.example.test/{z}/{x}/{y}.jpg"
		),
		settings
	);
	QVERIFY2(result.isDirect(), qPrintable(result.detail));
	auto const& source = *result.source;
	QCOMPARE(source.metadata.id, settings.id);
	QCOMPARE(source.metadata.name, settings.name);
	QCOMPARE(source.row_scheme, imagery::TileRowScheme::Tms);
	QCOMPARE(source.min_zoom, 1);
	QCOMPARE(source.max_zoom, 3);
	QCOMPARE(source.media_type, QStringLiteral("image/jpeg"));
	QCOMPARE(source.request.referer, settings.referer);
	QCOMPARE(
		source.request.empty_http_status_codes,
		QVector<int>({ 404 })
	);
	QCOMPARE(source.notices.attribution_text, settings.attribution_text);
	QCOMPARE(source.notices.attribution_url, settings.attribution_url);
	QCOMPARE(
		source.tile_matrix_set.matrices.first().tile_size,
		QSize(512, 512)
	);
	auto const bounds =
		source.tile_matrix_set.matrices.first().tileBounds(0, 0);
	QVERIFY(bounds.isValid());
	QVERIFY(std::abs(bounds.west + 20037508.342789244) < 1.0e-8);
	QVERIFY(std::abs(bounds.east - 20037508.342789244) < 1.0e-8);
	QString error;
	QCOMPARE(
		source.tileUrl(0, 2, 1, 0, &error),
		QUrl(QStringLiteral(
			"https://tiles.example.test/2/1/3.jpg"
		))
	);
}

void ManualImagerySourceTest::classifiesDiscoveryAndUnsupportedServices()
{
	auto arcgis = imagery::ManualImagerySource::classify(
		QStringLiteral(
			"https://gis.example.test/arcgis/rest/services/"
			"County/Aerial/MapServer/tile/{z}/{y}/{x}"
			"?token=redacted&f=json"
		)
	);
	QCOMPARE(
		arcgis.outcome,
		imagery::ManualImageryOutcome::NeedsDiscovery
	);
	QCOMPARE(
		arcgis.input_kind,
		imagery::ManualImageryInputKind::ArcGisMapServer
	);
	QCOMPARE(arcgis.suggested_name, QStringLiteral("Aerial"));
	QCOMPARE(
		arcgis.service_url.path(),
		QStringLiteral(
			"/arcgis/rest/services/County/Aerial/MapServer"
		)
	);
	auto service_query = QUrlQuery(arcgis.service_url);
	QCOMPARE(
		service_query.queryItemValue(QStringLiteral("token")),
		QStringLiteral("redacted")
	);
	QVERIFY(!service_query.hasQueryItem(QStringLiteral("f")));
	auto discovery_query = QUrlQuery(arcgis.discovery_url);
	QCOMPARE(
		discovery_query.queryItemValue(QStringLiteral("f")),
		QStringLiteral("pjson")
	);

	auto image_server = imagery::ManualImagerySource::classify(
		QStringLiteral(
			"https://gis.example.test/arcgis/rest/services/"
			"Elevation/ImageServer"
		)
	);
	QCOMPARE(
		image_server.outcome,
		imagery::ManualImageryOutcome::NeedsDiscovery
	);
	QCOMPARE(
		image_server.input_kind,
		imagery::ManualImageryInputKind::ArcGisImageServer
	);
	QVERIFY(!image_server.service_url.hasQuery());
	QCOMPARE(
		QUrlQuery(image_server.discovery_url)
			.queryItemValue(QStringLiteral("f")),
		QStringLiteral("pjson")
	);

	for (auto const& item : {
		std::pair {
			QStringLiteral(
				"https://maps.example.test/ows?service=WMS&request=GetMap"
			),
			imagery::ManualImageryInputKind::Wms,
		},
		std::pair {
			QStringLiteral("https://maps.example.test/wmts"),
			imagery::ManualImageryInputKind::Wmts,
		},
		std::pair {
			QStringLiteral(
				"https://data.example.test/orthophoto.tiff"
			),
			imagery::ManualImageryInputKind::CloudOptimizedGeoTiff,
		},
	})
	{
		auto const result =
			imagery::ManualImagerySource::classify(item.first);
		QCOMPARE(
			result.outcome,
			imagery::ManualImageryOutcome::Unsupported
		);
		QCOMPARE(result.input_kind, item.second);
		QVERIFY(!result.detail.isEmpty());
		QVERIFY(!result.source);
	}
}


void ManualImagerySourceTest::
	tiledPlaceholdersOverrideServicePathHeuristics()
{
	for (auto const& path : {
		QStringLiteral("wms"),
		QStringLiteral("wmts"),
	})
	{
		auto const result = imagery::ManualImagerySource::classify(
			QStringLiteral(
				"https://tiles.example.test/%1/{z}/{x}/{y}.png")
				.arg(path));
		QVERIFY2(result.isDirect(), qPrintable(result.detail));
		QCOMPARE(
			result.input_kind,
			imagery::ManualImageryInputKind::TiledUrlTemplate);
	}
}


void ManualImagerySourceTest::rejectsInvalidInputs_data()
{
	QTest::addColumn<QString>("input");
	QTest::addColumn<int>("maximum_zoom");
	QTest::addColumn<int>("tile_size");
	QTest::newRow("empty") << QString() << 3 << 256;
	QTest::newRow("relative")
		<< QStringLiteral("/{z}/{x}/{y}.png") << 3 << 256;
	QTest::newRow("file")
		<< QStringLiteral("file:///tmp/{z}/{x}/{y}.png") << 3 << 256;
	QTest::newRow("userinfo")
		<< QStringLiteral(
			"https://user:secret@example.test/{z}/{x}/{y}.png"
		) << 3 << 256;
	QTest::newRow("empty-userinfo")
		<< QStringLiteral(
			"https://@example.test/{z}/{x}/{y}.png"
		) << 3 << 256;
	QTest::newRow("fragment")
		<< QStringLiteral(
			"https://example.test/{z}/{x}/{y}.png#fragment"
		) << 3 << 256;
	QTest::newRow("missing-placeholder")
		<< QStringLiteral(
			"https://example.test/{z}/{x}/tile.png"
		) << 3 << 256;
	QTest::newRow("unknown-placeholder")
		<< QStringLiteral(
			"https://example.test/{z}/{x}/{y}/{s}.png"
		) << 3 << 256;
	QTest::newRow("plain-url")
		<< QStringLiteral("https://example.test/tiles") << 3 << 256;
	QTest::newRow("arcgis-operation")
		<< QStringLiteral(
			"https://gis.example.test/arcgis/rest/services/"
			"County/Aerial/MapServer/export/{z}/{x}/{y}"
		) << 3 << 256;
	QTest::newRow("arcgis-layer")
		<< QStringLiteral(
			"https://gis.example.test/arcgis/rest/services/"
			"County/Aerial/MapServer/0"
		) << 3 << 256;
	QTest::newRow("zoom-too-high")
		<< QStringLiteral(
			"https://example.test/{z}/{x}/{y}.png"
		) << 31 << 256;
	QTest::newRow("unsupported-size")
		<< QStringLiteral(
			"https://example.test/{z}/{x}/{y}.png"
		) << 3 << 300;
}

void ManualImagerySourceTest::rejectsInvalidInputs()
{
	QFETCH(QString, input);
	QFETCH(int, maximum_zoom);
	QFETCH(int, tile_size);
	imagery::ManualTiledSourceSettings settings;
	settings.max_zoom = maximum_zoom;
	settings.tile_size = tile_size;
	auto const result =
		imagery::ManualImagerySource::classify(input, settings);
	QCOMPARE(
		result.outcome,
		imagery::ManualImageryOutcome::Invalid
	);
	QVERIFY(!result.source);
	QVERIFY(!result.detail.isEmpty());
}

void ManualImagerySourceTest::exposesSecretWarningsAndForbidsRecents()
{
	auto const result = imagery::ManualImagerySource::classify(
		QStringLiteral(
			"https://tiles.example.test/{z}/{x}/{y}.png"
			"?style=a%2Fb&%61pi_key=redacted"
			"&X-Amz-Signature=redacted"
		)
	);
	QVERIFY(result.isDirect());
	QCOMPARE(result.warnings.size(), 1);
	QCOMPARE(
		result.warnings.first(),
		imagery::ManualImageryWarning::LikelySecretQueryParameters
	);
	QCOMPARE(result.likely_secret_parameters.size(), 2);
	QVERIFY(result.likely_secret_parameters.contains(
		QStringLiteral("api_key"), Qt::CaseInsensitive
	));
	QVERIFY(result.likely_secret_parameters.contains(
		QStringLiteral("X-Amz-Signature"), Qt::CaseInsensitive
	));
	QVERIFY(!result.likely_secret_parameters.contains(
		QStringLiteral("style"), Qt::CaseInsensitive
	));
	QVERIFY(result.source->tile_urls.first().value.contains(
		QStringLiteral("style=a%2Fb")
	));
	QVERIFY(
		!imagery::ManualImageryDiscoveryResult::
			permitsRecentPersistence()
	);
}

QTEST_APPLESS_MAIN(ManualImagerySourceTest)
