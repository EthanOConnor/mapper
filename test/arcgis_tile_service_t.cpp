/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "arcgis_tile_service_t.h"

#include <functional>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <QtTest>

#include "imagery/arcgis_tile_service.h"

using namespace OpenOrienteering;

namespace {

QJsonObject spatialReference(int wkid, int latest_wkid = 0)
{
	QJsonObject result {
		{ QStringLiteral("wkid"), wkid },
	};
	if (latest_wkid > 0)
		result.insert(QStringLiteral("latestWkid"), latest_wkid);
	return result;
}

QJsonObject lod(int level, double resolution)
{
	return {
		{ QStringLiteral("level"), level },
		{ QStringLiteral("resolution"), resolution },
		{ QStringLiteral("scale"), resolution / 0.00028 },
	};
}

QJsonObject metadata(
	const QJsonObject& crs,
	double origin_x,
	double origin_y,
	int columns,
	int rows,
	const QJsonArray& lods,
	double west,
	double south,
	double east,
	double north,
	const QString& format = QStringLiteral("PNG32"))
{
	return {
		{ QStringLiteral("name"), QStringLiteral("Published service title") },
		{ QStringLiteral("singleFusedMapCache"), true },
		{ QStringLiteral("spatialReference"), crs },
		{ QStringLiteral("copyrightText"),
		  QStringLiteral("Published attribution") },
		{ QStringLiteral("tileInfo"), QJsonObject {
			{ QStringLiteral("rows"), rows },
			{ QStringLiteral("cols"), columns },
			{ QStringLiteral("format"), format },
			{ QStringLiteral("origin"), QJsonObject {
				{ QStringLiteral("x"), origin_x },
				{ QStringLiteral("y"), origin_y },
			} },
			{ QStringLiteral("spatialReference"), crs },
			{ QStringLiteral("lods"), lods },
		} },
		{ QStringLiteral("fullExtent"), QJsonObject {
			{ QStringLiteral("xmin"), west },
			{ QStringLiteral("ymin"), south },
			{ QStringLiteral("xmax"), east },
			{ QStringLiteral("ymax"), north },
			{ QStringLiteral("spatialReference"), crs },
		} },
	};
}

QByteArray bytes(const QJsonObject& object)
{
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray changed(
	const QJsonObject& base,
	const std::function<void(QJsonObject&)>& change)
{
	auto copy = base;
	change(copy);
	return bytes(copy);
}

QJsonObject basicWebMercator()
{
	constexpr auto half_world = 20037508.342789244;
	constexpr auto base_resolution = 156543.03392804097;
	return metadata(
		spatialReference(102100, 3857),
		-half_world, half_world, 256, 256,
		QJsonArray {
			lod(2, base_resolution / 4),
			lod(0, base_resolution),
			lod(1, base_resolution / 2),
		},
		-half_world - 500.0,
		-half_world - 1000.0,
		half_world + 750.0,
		half_world + 250.0
	);
}

}  // namespace

void ArcGisTileServiceTest::
	resolvesCanonicalWebMercatorAndPreservesCredentials()
{
	auto const service_url = QUrl(QStringLiteral(
		"https://gis.example.test/arcgis/rest/services/"
		"World/MapServer?token=redacted&f=pjson&style=a%2Fb"
	));
	auto const result = imagery::ArcGisTileService::parse(
		bytes(basicWebMercator()), service_url
	);
	QVERIFY2(result.resolved(), qPrintable(result.detail));
	QCOMPARE(result.service_title,
	         QStringLiteral("Published service title"));
	QCOMPARE(
		result.likely_secret_parameters,
		QStringList { QStringLiteral("token") }
	);
	auto const& source = *result.source;
	QCOMPARE(source.tile_matrix_set.id,
	         QStringLiteral("WebMercatorQuad"));
	QCOMPARE(source.tile_matrix_set.crs, QStringLiteral("EPSG:3857"));
	QCOMPARE(source.tile_matrix_set.matrices.size(), 3);
	QCOMPARE(source.tile_matrix_set.matrices.at(0).matrix_width, 1);
	QCOMPARE(source.tile_matrix_set.matrices.at(1).matrix_width, 2);
	QCOMPARE(source.tile_matrix_set.matrices.at(2).matrix_width, 4);
	QCOMPARE(
		source.tile_matrix_set.matrices.first().point_of_origin,
		QPointF(-20037508.342789244, 20037508.342789244)
	);
	QCOMPARE(
		source.tile_matrix_set.matrices.first().cell_size,
		156543.03392804097
	);
	QVERIFY(source.tile_limits.isEmpty());
	QCOMPARE(source.metadata.name,
	         QStringLiteral("Published service title"));
	QCOMPARE(source.notices.attribution_text,
	         QStringLiteral("Published attribution"));
	QCOMPARE(source.row_scheme, imagery::TileRowScheme::Xyz);
	QCOMPARE(source.media_type, QStringLiteral("image/png"));
	QVERIFY(source.tile_urls.first().value.contains(
		QStringLiteral("/tile/{z}/{y}/{x}")
	));
	QVERIFY(source.tile_urls.first().value.contains(
		QStringLiteral("token=redacted")
	));
	QVERIFY(source.tile_urls.first().value.contains(
		QStringLiteral("style=a%2Fb")
	));
	QVERIFY(source.tile_urls.first().value.endsWith(
		QStringLiteral("?token=redacted&style=a%2Fb")
	));
	QVERIFY(!source.tile_urls.first().value.contains(
		QStringLiteral("f=pjson")
	));
	QString error;
	auto const tile_url = source.tileUrl(0, 2, 1, 3, &error);
	QVERIFY2(tile_url.isValid(), qPrintable(error));
	QCOMPARE(
		tile_url.path(),
		QStringLiteral(
			"/arcgis/rest/services/World/MapServer/tile/2/3/1"
		)
	);
	auto const query = QUrlQuery(tile_url);
	QCOMPARE(
		query.queryItemValue(QStringLiteral("token")),
		QStringLiteral("redacted")
	);
	QCOMPARE(
		query.queryItemValue(
			QStringLiteral("style"), QUrl::FullyDecoded
		),
		QStringLiteral("a/b")
	);
}

void ArcGisTileServiceTest::resolvesCustomGridWithOriginBasedLimits()
{
	constexpr auto origin_x = 890000.0;
	constexpr auto origin_y = 967000.0;
	constexpr auto base_resolution = 1219.2;
	constexpr auto tile_span = base_resolution * 256;
	auto service = metadata(
		spatialReference(2927),
		origin_x, origin_y, 256, 256,
		QJsonArray {
			lod(0, base_resolution),
			lod(1, base_resolution / 2),
			lod(2, base_resolution / 4),
		},
		origin_x + 0.5 * tile_span,
		origin_y - 3.2 * tile_span,
		origin_x + 7.25 * tile_span,
		origin_y - 0.25 * tile_span,
		QStringLiteral("MIXED")
	);
	service.insert(QStringLiteral("cacheType"), QStringLiteral("Map"));
	imagery::ArcGisTileServiceSettings settings;
	settings.name = QStringLiteral("Local aerial");
	settings.referer =
		QUrl(QStringLiteral("https://maps.example.test/"));
	settings.empty_http_status_codes = { 404 };
	settings.attribution_text = QStringLiteral("Local publisher");

	auto const result = imagery::ArcGisTileService::parse(
		bytes(service),
		QUrl(QStringLiteral(
			"https://gis.example.test/arcgis/rest/services/"
			"Pierce/Aerial/ImageServer"
		)),
		settings
	);
	QVERIFY2(result.resolved(), qPrintable(result.detail));
	auto const& source = *result.source;
	QCOMPARE(source.metadata.name, settings.name);
	QCOMPARE(source.notices.attribution_text,
	         settings.attribution_text);
	QCOMPARE(source.request.referer, settings.referer);
	QCOMPARE(source.request.empty_http_status_codes,
	         QVector<int>({ 404 }));
	QCOMPARE(source.tile_matrix_set.crs, QStringLiteral("EPSG:2927"));
	QCOMPARE(source.tile_matrix_set.matrices.at(0).matrix_width, 8);
	QCOMPARE(source.tile_matrix_set.matrices.at(0).matrix_height, 4);
	QCOMPARE(source.tile_matrix_set.matrices.at(1).matrix_width, 16);
	QCOMPARE(source.tile_matrix_set.matrices.at(1).matrix_height, 8);
	QCOMPARE(source.tile_matrix_set.matrices.at(2).matrix_width, 32);
	QCOMPARE(source.tile_matrix_set.matrices.at(2).matrix_height, 16);
	auto const* limits = source.limitsForZoom(2);
	QVERIFY(limits);
	QCOMPARE(limits->min_column, 2);
	QCOMPARE(limits->max_column, 28);
	QCOMPARE(limits->min_row, 1);
	QCOMPARE(limits->max_row, 12);
	QCOMPARE(source.media_type, QStringLiteral("image/png"));
	QString error;
	QVERIFY2(source.validate(&error), qPrintable(error));
}

void ArcGisTileServiceTest::preservesRectangularTilesAndLodRange()
{
	auto service = metadata(
		spatialReference(2927),
		0, 2000, 256, 512,
		QJsonArray {
			lod(2, 25),
			lod(0, 100),
			lod(1, 50),
		},
		0, 2000 - 3 * 100 * 512,
		2 * 100 * 256, 2000
	);
	service.insert(QStringLiteral("minLOD"), 1);
	service.insert(QStringLiteral("maxLOD"), 2);
	auto const result = imagery::ArcGisTileService::parse(
		bytes(service),
		QUrl(QStringLiteral(
			"https://gis.example.test/arcgis/rest/services/"
			"Rectangular/MapServer"
		))
	);
	QVERIFY2(result.resolved(), qPrintable(result.detail));
	auto const& source = *result.source;
	QCOMPARE(source.min_zoom, 1);
	QCOMPARE(source.max_zoom, 2);
	QCOMPARE(
		source.tile_matrix_set.matrices.first().tile_size,
		QSize(256, 512)
	);
	QCOMPARE(source.tile_matrix_set.matrices.first().matrix_width, 2);
	QCOMPARE(source.tile_matrix_set.matrices.first().matrix_height, 3);
	QString error;
	QVERIFY2(source.validate(&error), qPrintable(error));
}

void ArcGisTileServiceTest::rejectsUnsupportedImageServerCacheType()
{
	auto service = basicWebMercator();
	service.insert(
		QStringLiteral("cacheType"), QStringLiteral("Elevation")
	);
	auto const result = imagery::ArcGisTileService::parse(
		bytes(service),
		QUrl(QStringLiteral(
			"https://gis.example.test/arcgis/rest/services/"
			"Elevation/ImageServer"
		))
	);
	QCOMPARE(
		result.outcome,
		imagery::ArcGisTileServiceOutcome::Unsupported
	);
	QVERIFY(!result.source);
	QVERIFY(result.detail.contains(QStringLiteral("elevation")));
}

void ArcGisTileServiceTest::
	returnsUnsupportedForValidUnusableServices_data()
{
	QTest::addColumn<QByteArray>("metadata");
	auto const base = basicWebMercator();
	QTest::newRow("dynamic-map")
		<< changed(base, [](QJsonObject& object) {
			object.insert(QStringLiteral("singleFusedMapCache"), false);
		});
	QTest::newRow("missing-tile-info")
		<< changed(base, [](QJsonObject& object) {
			object.remove(QStringLiteral("tileInfo"));
		});
	QTest::newRow("runtime-tile-profile")
		<< changed(base, [](QJsonObject& object) {
			auto tile_info =
				object.value(QStringLiteral("tileInfo")).toObject();
			tile_info.insert(
				QStringLiteral("cols"),
				imagery::maximum_runtime_tile_dimension);
			tile_info.insert(
				QStringLiteral("rows"),
				int(imagery::maximum_runtime_tile_pixels
				    / imagery::maximum_runtime_tile_dimension)
					+ 1);
			object.insert(
				QStringLiteral("tileInfo"),
				tile_info);
		});
	QTest::newRow("non-dyadic")
		<< changed(base, [](QJsonObject& object) {
			auto tile_info =
				object.value(QStringLiteral("tileInfo")).toObject();
			auto lods =
				tile_info.value(QStringLiteral("lods")).toArray();
			auto changed_lod = lods.at(0).toObject();
			changed_lod.insert(
				QStringLiteral("resolution"),
				changed_lod.value(
					QStringLiteral("resolution")).toDouble() * 1.1
			);
			lods.replace(0, changed_lod);
			tile_info.insert(QStringLiteral("lods"), lods);
			object.insert(QStringLiteral("tileInfo"), tile_info);
		});
	QTest::newRow("gapped-lods")
		<< changed(base, [](QJsonObject& object) {
			auto tile_info =
				object.value(QStringLiteral("tileInfo")).toObject();
			auto lods =
				tile_info.value(QStringLiteral("lods")).toArray();
			lods.removeAt(2);
			tile_info.insert(QStringLiteral("lods"), lods);
			object.insert(QStringLiteral("tileInfo"), tile_info);
		});
	QTest::newRow("esri-only-wkid")
		<< changed(base, [](QJsonObject& object) {
			auto const crs = spatialReference(102999);
			object.insert(QStringLiteral("spatialReference"), crs);
			auto tile_info =
				object.value(QStringLiteral("tileInfo")).toObject();
			tile_info.insert(QStringLiteral("spatialReference"), crs);
			object.insert(QStringLiteral("tileInfo"), tile_info);
			auto extent =
				object.value(QStringLiteral("fullExtent")).toObject();
			extent.insert(QStringLiteral("spatialReference"), crs);
			object.insert(QStringLiteral("fullExtent"), extent);
		});
	QTest::newRow("wkt-only")
		<< changed(base, [](QJsonObject& object) {
			QJsonObject crs {
				{ QStringLiteral("wkt"),
				  QStringLiteral("LOCAL_CS[\"Synthetic\"]") },
			};
			object.insert(QStringLiteral("spatialReference"), crs);
			auto tile_info =
				object.value(QStringLiteral("tileInfo")).toObject();
			tile_info.insert(QStringLiteral("spatialReference"), crs);
			object.insert(QStringLiteral("tileInfo"), tile_info);
		});
	QTest::newRow("foreign-extent-crs")
		<< changed(base, [](QJsonObject& object) {
			auto extent =
				object.value(QStringLiteral("fullExtent")).toObject();
			extent.insert(
				QStringLiteral("spatialReference"),
				spatialReference(2927)
			);
			object.insert(QStringLiteral("fullExtent"), extent);
		});
	QTest::newRow("unknown-format")
		<< changed(base, [](QJsonObject& object) {
			auto tile_info =
				object.value(QStringLiteral("tileInfo")).toObject();
			tile_info.insert(QStringLiteral("format"),
			                 QStringLiteral("LERC"));
			object.insert(QStringLiteral("tileInfo"), tile_info);
		});
	QTest::newRow("overflowing-extent")
		<< changed(base, [](QJsonObject& object) {
			auto extent =
				object.value(QStringLiteral("fullExtent")).toObject();
			extent.insert(QStringLiteral("xmax"), 1.0e300);
			object.insert(QStringLiteral("fullExtent"), extent);
			auto tile_info =
				object.value(QStringLiteral("tileInfo")).toObject();
			auto origin =
				tile_info.value(QStringLiteral("origin")).toObject();
			origin.insert(QStringLiteral("x"), 0);
			origin.insert(QStringLiteral("y"), 1.0e300);
			tile_info.insert(QStringLiteral("origin"), origin);
			tile_info.insert(
				QStringLiteral("spatialReference"),
				spatialReference(2927)
			);
			object.insert(QStringLiteral("tileInfo"), tile_info);
			object.insert(
				QStringLiteral("spatialReference"),
				spatialReference(2927)
			);
			extent.insert(
				QStringLiteral("spatialReference"),
				spatialReference(2927)
			);
			object.insert(QStringLiteral("fullExtent"), extent);
		});
}

void ArcGisTileServiceTest::
	returnsUnsupportedForValidUnusableServices()
{
	QFETCH(QByteArray, metadata);
	auto const result = imagery::ArcGisTileService::parse(
		metadata,
		QUrl(QStringLiteral(
			"https://gis.example.test/arcgis/rest/services/"
			"World/MapServer"
		))
	);
	QCOMPARE(
		result.outcome,
		imagery::ArcGisTileServiceOutcome::Unsupported
	);
	QVERIFY(!result.source);
	QVERIFY(!result.detail.isEmpty());
}

void ArcGisTileServiceTest::rejectsMalformedMetadata_data()
{
	QTest::addColumn<QByteArray>("metadata");
	QTest::addColumn<QUrl>("service_url");
	auto const url = QUrl(QStringLiteral(
		"https://gis.example.test/arcgis/rest/services/World/MapServer"
	));
	auto const base = basicWebMercator();
	QTest::newRow("bad-json") << QByteArray("{") << url;
	QTest::newRow("arcgis-error")
		<< bytes(QJsonObject {
			{ QStringLiteral("error"), QJsonObject {
				{ QStringLiteral("code"), 498 },
				{ QStringLiteral("message"),
				  QStringLiteral("Invalid token redacted") },
			} },
		}) << url;
	QTest::newRow("bad-url")
		<< bytes(base)
		<< QUrl(QStringLiteral("https://gis.example.test/not-a-service"));
	QTest::newRow("zero-rows")
		<< changed(base, [](QJsonObject& object) {
			auto tile_info =
				object.value(QStringLiteral("tileInfo")).toObject();
			tile_info.insert(QStringLiteral("rows"), 0);
			object.insert(QStringLiteral("tileInfo"), tile_info);
		}) << url;
	QTest::newRow("duplicate-level")
		<< changed(base, [](QJsonObject& object) {
			auto tile_info =
				object.value(QStringLiteral("tileInfo")).toObject();
			auto lods =
				tile_info.value(QStringLiteral("lods")).toArray();
			lods.replace(0, lods.at(1));
			tile_info.insert(QStringLiteral("lods"), lods);
			object.insert(QStringLiteral("tileInfo"), tile_info);
		}) << url;
	QTest::newRow("inverted-extent")
		<< changed(base, [](QJsonObject& object) {
			auto extent =
				object.value(QStringLiteral("fullExtent")).toObject();
			extent.insert(QStringLiteral("xmax"),
			              extent.value(QStringLiteral("xmin")));
			object.insert(QStringLiteral("fullExtent"), extent);
		}) << url;
	QTest::newRow("bad-min-lod")
		<< changed(base, [](QJsonObject& object) {
			object.insert(QStringLiteral("minLOD"), 9);
		}) << url;
	QTest::newRow("bad-fused-type")
		<< changed(base, [](QJsonObject& object) {
			object.insert(QStringLiteral("singleFusedMapCache"),
			              QStringLiteral("yes"));
		}) << url;
	QTest::newRow("bad-format-type")
		<< changed(base, [](QJsonObject& object) {
			auto tile_info =
				object.value(QStringLiteral("tileInfo")).toObject();
			tile_info.insert(QStringLiteral("format"), 7);
			object.insert(QStringLiteral("tileInfo"), tile_info);
		}) << url;
}

void ArcGisTileServiceTest::rejectsMalformedMetadata()
{
	QFETCH(QByteArray, metadata);
	QFETCH(QUrl, service_url);
	auto const result =
		imagery::ArcGisTileService::parse(metadata, service_url);
	QCOMPARE(
		result.outcome,
		imagery::ArcGisTileServiceOutcome::Invalid
	);
	QVERIFY(!result.source);
	QVERIFY(!result.detail.isEmpty());
	QVERIFY(!result.detail.contains(QStringLiteral("redacted")));
}

void ArcGisTileServiceTest::rejectsOversizedMetadata()
{
	QByteArray oversized(
		imagery::ArcGisTileService::maximum_metadata_size + 1, ' '
	);
	auto const result = imagery::ArcGisTileService::parse(
		oversized,
		QUrl(QStringLiteral(
			"https://gis.example.test/arcgis/rest/services/"
			"World/MapServer"
		))
	);
	QCOMPARE(
		result.outcome,
		imagery::ArcGisTileServiceOutcome::Invalid
	);
	QVERIFY(result.detail.contains(QStringLiteral("1 MiB")));
}

QTEST_APPLESS_MAIN(ArcGisTileServiceTest)
