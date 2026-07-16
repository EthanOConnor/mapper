/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "imagery_core_t.h"

#include <cmath>

#include <QtTest>
#include <QCryptographicHash>
#include <QJsonDocument>

#include "imagery/imagery_source.h"
#include "imagery/imagery_source_snapshot.h"
#include "imagery/tile_matrix_set.h"

using namespace OpenOrienteering;

namespace {

imagery::ResolvedImagerySource sourceFixture()
{
	imagery::ResolvedImagerySource source;
	source.metadata.id = QStringLiteral("org.example.aerial-2026");
	source.metadata.name = QStringLiteral("Example Aerial 2026");
	source.metadata.description = QStringLiteral("Synthetic source for imagery core tests");
	source.metadata.category = imagery::ImageryCategory::Aerial;
	source.metadata.start_date = QDate(2026, 1, 1);
	source.metadata.end_date = QDate(2026, 6, 30);
	source.notices.attribution_text = QStringLiteral("Example Mapping Club");
	source.notices.attribution_url = QUrl(QStringLiteral("https://example.test/attribution"));
	source.notices.source_url = QUrl(QStringLiteral("https://example.test/source"));
	source.notices.terms_url = QUrl(QStringLiteral("https://example.test/terms"));
	source.tile_urls = {
		{ QStringLiteral("https://tiles-a.example.test/aerial/{z}/{x}/{y}.png?style=base") },
		{ QStringLiteral("https://tiles-b.example.test/aerial/{z}/{x}/{y}.png?style=base") },
	};
	source.row_scheme = imagery::TileRowScheme::Xyz;
	source.media_type = QStringLiteral("image/png");
	source.tile_matrix_set = imagery::TileMatrixSet::webMercatorQuad();
	source.min_zoom = 0;
	source.max_zoom = 20;
	source.tile_limits = {
		{ 3, 1, 6, 0, 7 },
	};
	source.request.referer = QUrl(QStringLiteral("https://example.test/map/"));
	source.request.empty_http_status_codes = { 204, 404 };
	source.catalog_provenance = imagery::CatalogSourceProvenance {
		QStringLiteral("org.example.imagery"),
		7,
		QByteArray(64, 'a'),
		source.metadata.id,
		QByteArray(64, 'b'),
		QByteArray(64, 'c'),
	};
	source.registration = imagery::TranslationRegistration {
		QStringLiteral("EPSG:3857"),
		QStringLiteral("EPSG:3857"),
		QStringLiteral("org.example.survey-frame-2026"),
		-0.42,
		0.17,
		{
			QStringLiteral("survey-control"),
			QDate(2026, 6, 20),
			QStringLiteral("Example Mapping Team"),
			0.12,
			QStringLiteral("Fit from six synthetic control points"),
		},
	};
	return source;
}

}  // namespace

void ImageryCoreTest::webMercatorQuadIsDyadic()
{
	auto const matrix_set = imagery::TileMatrixSet::webMercatorQuad();
	QString error;
	QVERIFY2(matrix_set.validateDyadicTopLeft(&error), qPrintable(error));
	QCOMPARE(matrix_set.id, QStringLiteral("WebMercatorQuad"));
	QCOMPARE(matrix_set.crs, QStringLiteral("EPSG:3857"));
	QCOMPARE(matrix_set.matrices.size(), 25);

	auto const* zoom_zero = matrix_set.matrixForZoom(0);
	auto const* zoom_24 = matrix_set.matrixForZoom(24);
	QVERIFY(zoom_zero);
	QVERIFY(zoom_24);
	QCOMPARE(zoom_zero->tile_size, QSize(256, 256));
	QCOMPARE(zoom_zero->matrix_width, qint64(1));
	QCOMPARE(zoom_24->matrix_width, qint64(1) << 24);
	QVERIFY(std::abs(zoom_zero->cell_size / zoom_24->cell_size - double(qint64(1) << 24)) < 1.0e-6);

	auto const bounds = zoom_zero->tileBounds(0, 0);
	QVERIFY(bounds.isValid());
	constexpr auto half_world = 20037508.342789244;
	QVERIFY(std::abs(bounds.west + half_world) < 1.0e-6);
	QVERIFY(std::abs(bounds.south + half_world) < 1.0e-6);
	QVERIFY(std::abs(bounds.east - half_world) < 1.0e-6);
	QVERIFY(std::abs(bounds.north - half_world) < 1.0e-6);
}

void ImageryCoreTest::expandsXyzAndTmsRows()
{
	auto source = sourceFixture();
	QString error;
	QVERIFY2(source.validate(&error), qPrintable(error));

	auto xyz = source.tileUrl(0, 3, 2, 1, &error);
	QVERIFY2(xyz.isValid(), qPrintable(error));
	QCOMPARE(
		xyz.toString(QUrl::FullyEncoded),
		QStringLiteral("https://tiles-a.example.test/aerial/3/2/1.png?style=base")
	);

	source.row_scheme = imagery::TileRowScheme::Tms;
	auto tms = source.tileUrl(1, 3, 2, 1, &error);
	QVERIFY2(tms.isValid(), qPrintable(error));
	QCOMPARE(
		tms.toString(QUrl::FullyEncoded),
		QStringLiteral("https://tiles-b.example.test/aerial/3/2/6.png?style=base")
	);

	QVERIFY(source.tileUrl(0, 3, 0, 1, &error).isEmpty());
	QVERIFY(error.contains(QStringLiteral("limits")));
	QVERIFY(source.tileUrl(0, 21, 2, 1, &error).isEmpty());
	QVERIFY(error.contains(QStringLiteral("usable range")));
}

void ImageryCoreTest::rejectsUnsafeUrlTemplates()
{
	auto accepts = [](const QString& text) {
		QString error;
		return imagery::TileUrlTemplate { text }.validate(&error);
	};

	QVERIFY(accepts(QStringLiteral("https://tiles.example.test/{z}/{x}/{y}.png")));
	QVERIFY(accepts(QStringLiteral("http://tiles.example.test/tiles?z={z}&x={x}&y={y}")));
	QVERIFY(!accepts(QStringLiteral("ftp://tiles.example.test/{z}/{x}/{y}.png")));
	QVERIFY(!accepts(QStringLiteral("https://user:secret@tiles.example.test/{z}/{x}/{y}.png")));
	QVERIFY(!accepts(QStringLiteral("https://tiles.example.test/{z}/{x}.png")));
	QVERIFY(!accepts(QStringLiteral("https://tiles.example.test/{z}/{x}/{y}/{y}.png")));
	QVERIFY(!accepts(QStringLiteral("https://tiles.example.test/${z}/${x}/${y}.png")));
	QVERIFY(!accepts(QStringLiteral("https://tiles.example.test/{z}/{x}/{y}/{s}.png")));
	QVERIFY(!accepts(QStringLiteral("https://{z}.example.test/tiles/{x}/{y}.png")));
	QVERIFY(!accepts(QStringLiteral("https://tiles.example.test/{z}/{x}/{y}.png#fragment")));
	QVERIFY(!accepts(QStringLiteral("https://tiles.example.test/{z}/{x}/{y}.png\nInjected: value")));
}

void ImageryCoreTest::validatesTileLimits()
{
	auto source = sourceFixture();
	QString error;
	QVERIFY2(source.validate(&error), qPrintable(error));

	source.tile_limits.push_back({ 3, 0, 1, 0, 1 });
	QVERIFY(!source.validate(&error));
	QVERIFY(error.contains(QStringLiteral("duplicate zoom")));

	source = sourceFixture();
	source.tile_limits = { { 3, 0, 8, 0, 7 } };
	QVERIFY(!source.validate(&error));
	QVERIFY(error.contains(QStringLiteral("outside")));

	source = sourceFixture();
	source.tile_matrix_set.matrices[4].cell_size *= 0.9;
	QVERIFY(!source.validate(&error));
	QVERIFY(error.contains(QStringLiteral("halve")));
}

void ImageryCoreTest::requestPolicyHasExplicitDefaults()
{
	imagery::ImageryRequestPolicy policy;
	QCOMPARE(policy.empty_http_status_codes, QVector<int>({ 204, 404 }));

	auto source = sourceFixture();
	QString error;
	source.catalog_provenance->catalog_sha256 = QByteArray(64, 'A');
	QVERIFY(!source.validate(&error));
	QVERIFY(error.contains(QStringLiteral("lowercase SHA-256")));
}

void ImageryCoreTest::snapshotRoundTripsDeterministically()
{
	auto const source = sourceFixture();
	QString error;
	auto const first = imagery::ImagerySourceSnapshotCodec::encode(source, &error);
	QVERIFY2(first, qPrintable(error));
	auto const second = imagery::ImagerySourceSnapshotCodec::encode(source, &error);
	QVERIFY2(second, qPrintable(error));
	QCOMPARE(first->canonical_json, second->canonical_json);
	QCOMPARE(first->sha256, second->sha256);
	QCOMPARE(first->sha256.size(), 64);
	QCOMPARE(
		first->sha256,
		QCryptographicHash::hash(
			first->canonical_json, QCryptographicHash::Sha256
		).toHex()
	);
	QVERIFY(!first->canonical_json.contains('\n'));
	QVERIFY(QJsonDocument::fromJson(first->canonical_json).isObject());

	auto const decoded = imagery::ImagerySourceSnapshotCodec::decode(
		first->canonical_json, &error
	);
	QVERIFY2(decoded, qPrintable(error));
	QCOMPARE(decoded->canonical_json, first->canonical_json);
	QCOMPARE(decoded->sha256, first->sha256);
	QCOMPARE(decoded->source, source);
	QVERIFY(decoded->source.catalog_provenance);
	QCOMPARE(decoded->source.catalog_provenance->catalog_revision, 7);
	QCOMPARE(decoded->source.catalog_provenance->operational_fingerprint, QByteArray(64, 'c'));

	auto const round_trip = imagery::ImagerySourceSnapshotCodec::encode(
		decoded->source, &error
	);
	QVERIFY2(round_trip, qPrintable(error));
	QCOMPARE(round_trip->canonical_json, first->canonical_json);
}

void ImageryCoreTest::snapshotRejectsUnsupportedRegistrations()
{
	QString error;
	auto const encoded = imagery::ImagerySourceSnapshotCodec::encode(
		sourceFixture(), &error
	);
	QVERIFY2(encoded, qPrintable(error));

	auto affine = encoded->canonical_json;
	QVERIFY(affine.contains("\"translation2d\""));
	affine.replace("\"translation2d\"", "\"affine2d\"");
	QVERIFY(!imagery::ImagerySourceSnapshotCodec::decode(affine, &error));
	QVERIFY(error.contains(QStringLiteral("does not support affine2d")));

	auto grid = encoded->canonical_json;
	QVERIFY(grid.contains("\"translation2d\""));
	grid.replace("\"translation2d\"", "\"gridShift\"");
	QVERIFY(!imagery::ImagerySourceSnapshotCodec::decode(grid, &error));
	QVERIFY(error.contains(QStringLiteral("does not support gridShift")));
}

QTEST_MAIN(ImageryCoreTest)
