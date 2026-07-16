/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "oic_catalog_t.h"

#include <functional>
#include <limits>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "test_config.h"

#include "imagery/oic_catalog.h"

using namespace OpenOrienteering;

namespace {

constexpr auto web_mercator_quad_uri =
	"http://www.opengis.net/def/tilematrixset/OGC/1.0/WebMercatorQuad";

QByteArray fixture(const QString& relative_path)
{
	QFile file(
		QString::fromUtf8(MAPPER_TEST_SOURCE_DIR)
		+ QStringLiteral("/data/imagery-catalogs/") + relative_path
	);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	return file.readAll();
}

QJsonObject minimalSource()
{
	return {
		{ QStringLiteral("id"), QStringLiteral("aerial") },
		{ QStringLiteral("name"), QStringLiteral("Example aerial") },
		{ QStringLiteral("type"), QStringLiteral("raster-tiles") },
		{ QStringLiteral("tiles"), QJsonArray {
			QStringLiteral(
				"https://tiles.example.test/aerial/{z}/{x}/{y}.png"
			),
		} },
		{ QStringLiteral("scheme"), QStringLiteral("xyz") },
		{ QStringLiteral("tileMatrixSetURI"),
		  QString::fromLatin1(web_mercator_quad_uri) },
	};
}

QJsonObject minimalCatalog()
{
	return {
		{ QStringLiteral("format"),
		  QStringLiteral("org.openorienteering.imagery-catalog") },
		{ QStringLiteral("version"), 1 },
		{ QStringLiteral("id"),
		  QStringLiteral("org.example.imagery.minimal") },
		{ QStringLiteral("revision"), 3 },
		{ QStringLiteral("name"), QStringLiteral("Minimal catalog") },
		{ QStringLiteral("sources"), QJsonArray { minimalSource() } },
	};
}

QByteArray compact(const QJsonObject& object)
{
	return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray changedCatalog(
	const std::function<void(QJsonObject&)>& change)
{
	auto catalog = minimalCatalog();
	change(catalog);
	return compact(catalog);
}

QByteArray changedSource(
	const std::function<void(QJsonObject&)>& change)
{
	return changedCatalog([&change](QJsonObject& catalog) {
		auto sources = catalog.value(QStringLiteral("sources")).toArray();
		auto source = sources.first().toObject();
		change(source);
		sources.replace(0, source);
		catalog.insert(QStringLiteral("sources"), sources);
	});
}

QJsonObject tileMatrix(
	int zoom,
	double cell_size,
	qint64 width,
	qint64 height)
{
	return {
		{ QStringLiteral("id"), QString::number(zoom) },
		{ QStringLiteral("scaleDenominator"), cell_size / 0.00028 },
		{ QStringLiteral("cellSize"), cell_size },
		{ QStringLiteral("pointOfOrigin"), QJsonArray { 1000.0, 5000.0 } },
		{ QStringLiteral("cornerOfOrigin"), QStringLiteral("topLeft") },
		{ QStringLiteral("tileWidth"), 256 },
		{ QStringLiteral("tileHeight"), 256 },
		{ QStringLiteral("matrixWidth"), double(width) },
		{ QStringLiteral("matrixHeight"), double(height) },
	};
}

QJsonObject inlineMatrixSet(bool dyadic = true)
{
	return {
		{ QStringLiteral("id"), QStringLiteral("org.example.synthetic-grid") },
		{ QStringLiteral("crs"), QStringLiteral("EPSG:2927") },
		{ QStringLiteral("orderedAxes"),
		  QJsonArray { QStringLiteral("E"), QStringLiteral("N") } },
		{ QStringLiteral("tileMatrices"), QJsonArray {
			tileMatrix(0, 100.0, 1, 1),
			tileMatrix(1, dyadic ? 50.0 : 40.0,
			           dyadic ? 2 : 3, dyadic ? 2 : 3),
			tileMatrix(2, dyadic ? 25.0 : 20.0,
			           dyadic ? 4 : 6, dyadic ? 4 : 6),
		} },
	};
}

QJsonObject translationRegistration()
{
	return {
		{ QStringLiteral("direction"),
		  QStringLiteral("source-to-corrected") },
		{ QStringLiteral("sourceFrame"), QJsonObject {
			{ QStringLiteral("crs"), QStringLiteral("EPSG:2927") },
		} },
		{ QStringLiteral("targetFrame"), QJsonObject {
			{ QStringLiteral("crs"), QStringLiteral("EPSG:2927") },
			{ QStringLiteral("id"),
			  QStringLiteral("org.example.survey-frame") },
		} },
		{ QStringLiteral("operation"), QJsonObject {
			{ QStringLiteral("type"), QStringLiteral("translation2d") },
			{ QStringLiteral("unit"), QStringLiteral("crs") },
			{ QStringLiteral("dx"), -0.42 },
			{ QStringLiteral("dy"), 0.17 },
		} },
		{ QStringLiteral("provenance"), QJsonObject {
			{ QStringLiteral("method"), QStringLiteral("survey-control") },
			{ QStringLiteral("observed"), QStringLiteral("2026-06-20") },
			{ QStringLiteral("author"), QStringLiteral("Example Mapping Team") },
			{ QStringLiteral("rmsError"), 0.12 },
			{ QStringLiteral("notes"),
			  QStringLiteral("Fit from synthetic control points") },
		} },
	};
}

bool hasDiagnostic(
	const imagery::OicCatalogReadResult& result,
	imagery::OicDiagnosticKind kind,
	const QString& fragment = {})
{
	for (auto const& diagnostic : result.diagnostics)
	{
		if (diagnostic.kind == kind
		    && (fragment.isEmpty()
		        || diagnostic.code.contains(fragment)
		        || diagnostic.path.contains(fragment)
		        || diagnostic.message.contains(fragment)))
		{
			return true;
		}
	}
	return false;
}

}  // namespace

void OicCatalogTest::parsesMinimalCatalog()
{
	auto const bytes = compact(minimalCatalog());
	auto const result = imagery::OicCatalogReader::read(bytes);
	QVERIFY2(
		result.accepted(),
		qPrintable(
			result.diagnostics.isEmpty()
			  ? QString()
			  : result.diagnostics.first().displayText()
		)
	);
	QCOMPARE(result.catalog.original_bytes, bytes);
	QCOMPARE(
		result.catalog.document_sha256,
		QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()
	);
	QCOMPARE(result.validSourceCount(), 1);
	QCOMPARE(result.supportedSourceCount(), 1);
	QCOMPARE(result.catalog.sources.size(), 1);

	auto const& definition = result.catalog.sources.first();
	QVERIFY(definition.valid);
	QVERIFY(definition.supported);
	QVERIFY(definition.resolved_source);
	QCOMPARE(definition.media_type, QStringLiteral("image/png"));
	QCOMPARE(
		definition.request.empty_http_status_codes,
		QVector<int>()
	);
	QCOMPARE(definition.full_fingerprint.size(), 64);
	QCOMPARE(definition.operational_fingerprint.size(), 64);
	QCOMPARE(
		definition.resolved_source->tile_matrix_set.crs,
		QStringLiteral("EPSG:3857")
	);
	QCOMPARE(definition.resolved_source->min_zoom, 0);
	QCOMPARE(definition.resolved_source->max_zoom, 24);
	QVERIFY(definition.resolved_source->catalog_provenance);
	QCOMPARE(
		definition.resolved_source->catalog_provenance->catalog_id,
		result.catalog.id
	);
	QCOMPARE(
		definition.resolved_source->catalog_provenance->catalog_revision,
		3
	);
	QCOMPARE(
		definition.resolved_source->catalog_provenance->catalog_sha256,
		result.catalog.document_sha256
	);
	QString error;
	QVERIFY2(
		definition.resolved_source->validate(&error),
		qPrintable(error)
	);
}

void OicCatalogTest::preservesHistoricalFixturesAndFingerprints()
{
	auto const minimal_bytes =
		fixture(QStringLiteral("valid/minimal.oic"));
	QVERIFY(!minimal_bytes.isEmpty());
	auto const minimal =
		imagery::OicCatalogReader::read(minimal_bytes);
	QVERIFY2(
		minimal.accepted(),
		qPrintable(
			minimal.diagnostics.isEmpty()
			  ? QString()
			  : minimal.diagnostics.first().displayText()
		)
	);
	auto const& minimal_source = minimal.catalog.sources.first();
	QCOMPARE(
		minimal_source.full_fingerprint,
		QByteArray(
			"231ea73d1cb066e5d67a56981b25ed649279ad4aec79d1569bf2f591d6dedbf4"
		)
	);
	QCOMPARE(
		minimal_source.operational_fingerprint,
		QByteArray(
			"f76f7b6b37eb68278ddc458cd72d1ebd40d23b7b5e8e1e4fa3edf3d5dfe46ed5"
		)
	);
	QVERIFY(minimal_source.request.empty_http_status_codes.isEmpty());
	QVERIFY(minimal_source.resolved_source);
	QVERIFY(
		minimal_source.resolved_source->request.empty_http_status_codes.isEmpty()
	);

	auto const custom_bytes =
		fixture(QStringLiteral("valid/custom-dyadic-epsg2927.oic"));
	QVERIFY(!custom_bytes.isEmpty());
	auto const custom =
		imagery::OicCatalogReader::read(custom_bytes);
	QVERIFY2(
		custom.accepted(),
		qPrintable(
			custom.diagnostics.isEmpty()
			  ? QString()
			  : custom.diagnostics.first().displayText()
		)
	);
	auto const& custom_source = custom.catalog.sources.first();
	QVERIFY(custom_source.supported);
	QCOMPARE(
		custom_source.original_object.value(QStringLiteral("tiles"))
			.toArray().first().toString(),
		QStringLiteral(
			"https://tiles.example.test/arcgis/rest/services/aerial/"
			"MapServer/tile/${z}/${y}/${x}"
		)
	);
	QCOMPARE(
		custom_source.tile_urls.first().value,
		QStringLiteral(
			"https://tiles.example.test/arcgis/rest/services/aerial/"
			"MapServer/tile/{z}/{y}/{x}"
		)
	);
	QCOMPARE(
		custom_source.request.empty_http_status_codes,
		QVector<int>({ 204, 404 })
	);
	QCOMPARE(custom_source.tile_limit_definitions.size(), 1);
	QCOMPARE(
		custom_source.tile_limit_definitions.first().tile_matrix,
		QStringLiteral("2")
	);
	QCOMPARE(custom_source.tile_limits.size(), 1);
	QVERIFY(custom_source.resolved_source);
}

void OicCatalogTest::acceptsDocumentedTemplateAliases()
{
	auto const canonical =
		imagery::OicCatalogReader::read(compact(minimalCatalog()));
	auto const aliases =
		imagery::OicCatalogReader::read(
			changedSource([](QJsonObject& source) {
				source.insert(QStringLiteral("tiles"), QJsonArray {
					QStringLiteral(
						"HTTPS://TILES.EXAMPLE.TEST:443/aerial/"
						"${z}/${x}/${y}.png"
					),
				});
			})
		);
	QVERIFY(canonical.accepted());
	QVERIFY2(
		aliases.accepted(),
		qPrintable(
			aliases.diagnostics.isEmpty()
			  ? QString()
			  : aliases.diagnostics.first().displayText()
		)
	);
	auto const& source = aliases.catalog.sources.first();
	QCOMPARE(
		source.original_object.value(QStringLiteral("tiles"))
			.toArray().first().toString(),
		QStringLiteral(
			"HTTPS://TILES.EXAMPLE.TEST:443/aerial/${z}/${x}/${y}.png"
		)
	);
	QCOMPARE(
		source.tile_urls.first().value,
		QStringLiteral(
			"HTTPS://TILES.EXAMPLE.TEST:443/aerial/{z}/{x}/{y}.png"
		)
	);
	QCOMPARE(
		source.full_fingerprint,
		canonical.catalog.sources.first().full_fingerprint
	);
	QCOMPARE(
		source.operational_fingerprint,
		canonical.catalog.sources.first().operational_fingerprint
	);
}

void OicCatalogTest::preservesMetadataNoticesLicenseAndTranslationProvenance()
{
	auto catalog = minimalCatalog();
	catalog.insert(
		QStringLiteral("description"),
		QStringLiteral("Synthetic catalog description")
	);
	catalog.insert(QStringLiteral("created"), QStringLiteral("2026-07-01"));
	catalog.insert(QStringLiteral("updated"), QStringLiteral("2026-07-16"));
	catalog.insert(QStringLiteral("catalogLicense"), QStringLiteral("CC0-1.0"));
	catalog.insert(QStringLiteral("publisher"), QJsonObject {
		{ QStringLiteral("name"), QStringLiteral("Example Mapping Club") },
		{ QStringLiteral("url"), QStringLiteral("https://example.test/") },
		{ QStringLiteral("contactUrl"),
		  QStringLiteral("https://example.test/contact") },
	});
	catalog.insert(QStringLiteral("extensions"), QJsonObject {
		{ QStringLiteral("org.example.catalog"), QJsonObject {
			{ QStringLiteral("reviewed"), true },
		} },
	});

	auto source = minimalSource();
	source.remove(QStringLiteral("tileMatrixSetURI"));
	source.insert(QStringLiteral("tileMatrixSet"), inlineMatrixSet());
	source.insert(QStringLiteral("description"),
	              QStringLiteral("Synthetic registered imagery"));
	source.insert(QStringLiteral("format"), QStringLiteral("image/jpeg"));
	source.insert(QStringLiteral("minTileMatrix"), QStringLiteral("0"));
	source.insert(QStringLiteral("maxTileMatrix"), QStringLiteral("2"));
	source.insert(QStringLiteral("category"), QStringLiteral("aerial"));
	source.insert(QStringLiteral("startDate"), QStringLiteral("2025-04-01"));
	source.insert(QStringLiteral("endDate"), QStringLiteral("2025-09-30"));
	source.insert(QStringLiteral("tileMatrixLimits"), QJsonArray {
		QJsonObject {
			{ QStringLiteral("tileMatrix"), QStringLiteral("2") },
			{ QStringLiteral("minTileRow"), 1 },
			{ QStringLiteral("maxTileRow"), 3 },
			{ QStringLiteral("minTileCol"), 0 },
			{ QStringLiteral("maxTileCol"), 2 },
		},
	});
	source.insert(QStringLiteral("request"), QJsonObject {
		{ QStringLiteral("referer"),
		  QStringLiteral("https://example.test/maps/") },
		{ QStringLiteral("emptyHttpStatusCodes"),
		  QJsonArray { 204, 404 } },
	});
	source.insert(QStringLiteral("notices"), QJsonObject {
		{ QStringLiteral("attributionText"),
		  QStringLiteral("Synthetic imagery") },
		{ QStringLiteral("attributionUrl"),
		  QStringLiteral("https://example.test/attribution") },
		{ QStringLiteral("sourceUrl"),
		  QStringLiteral("https://example.test/source") },
		{ QStringLiteral("termsUrl"),
		  QStringLiteral("https://example.test/terms") },
		{ QStringLiteral("privacyUrl"),
		  QStringLiteral("https://example.test/privacy") },
		{ QStringLiteral("notes"),
		  QStringLiteral("Publisher-supplied legal information") },
	});
	QJsonArray ring {
		QJsonArray { -122.9, 46.8 },
		QJsonArray { -122.0, 46.8 },
		QJsonArray { -122.0, 47.4 },
		QJsonArray { -122.9, 46.8 },
	};
	QJsonArray rings;
	rings.push_back(ring);
	source.insert(
		QStringLiteral("coverage"),
		QJsonObject {
			{ QStringLiteral("type"), QStringLiteral("Polygon") },
			{ QStringLiteral("coordinates"), rings },
		}
	);
	source.insert(
		QStringLiteral("registration"), translationRegistration()
	);
	source.insert(QStringLiteral("extensions"), QJsonObject {
		{ QStringLiteral("org.example.source"), QJsonObject {
			{ QStringLiteral("reviewedBy"), QStringLiteral("Test suite") },
		} },
	});
	catalog.insert(QStringLiteral("sources"), QJsonArray { source });

	auto const result = imagery::OicCatalogReader::read(compact(catalog));
	QVERIFY2(
		result.accepted(),
		qPrintable(
			result.diagnostics.isEmpty()
			  ? QString()
			  : result.diagnostics.first().displayText()
		)
	);
	QCOMPARE(result.catalog.catalog_license, QStringLiteral("CC0-1.0"));
	QVERIFY(result.catalog.publisher);
	QCOMPARE(
		result.catalog.publisher->name,
		QStringLiteral("Example Mapping Club")
	);
	QCOMPARE(
		result.catalog.publisher->contact_url,
		QUrl(QStringLiteral("https://example.test/contact"))
	);
	QVERIFY(result.catalog.extensions.contains(
		QStringLiteral("org.example.catalog")
	));

	auto const& definition = result.catalog.sources.first();
	QVERIFY(definition.supported);
	QCOMPARE(
		definition.metadata.description,
		QStringLiteral("Synthetic registered imagery")
	);
	QCOMPARE(
		definition.metadata.category, imagery::ImageryCategory::Aerial
	);
	QCOMPARE(
		definition.notices.attribution_text,
		QStringLiteral("Synthetic imagery")
	);
	QCOMPARE(
		definition.notices.privacy_url,
		QUrl(QStringLiteral("https://example.test/privacy"))
	);
	QCOMPARE(definition.tile_limits.size(), 1);
	QCOMPARE(definition.tile_limits.first().zoom, 2);
	QCOMPARE(
		definition.registration.kind,
		imagery::OicRegistrationKind::Translation2d
	);
	QCOMPARE(
		definition.registration.provenance.method,
		QStringLiteral("survey-control")
	);
	QCOMPARE(
		definition.registration.provenance.observed,
		QDate(2026, 6, 20)
	);
	QVERIFY(definition.coverage.contains(QStringLiteral("coordinates")));
	QVERIFY(definition.extensions.contains(
		QStringLiteral("org.example.source")
	));
	QVERIFY(definition.resolved_source);
	QVERIFY(definition.resolved_source->registration);
	QCOMPARE(definition.resolved_source->registration->dx, -0.42);
	QVERIFY(
		definition.resolved_source->registration->provenance.rms_error
	);
	QCOMPARE(
		*definition.resolved_source->registration->provenance.rms_error,
		0.12
	);
}

void OicCatalogTest::retainsAffineAsUnsupported()
{
	auto bytes = changedSource([](QJsonObject& source) {
		auto registration = QJsonObject {
			{ QStringLiteral("direction"),
			  QStringLiteral("source-to-corrected") },
			{ QStringLiteral("sourceFrame"), QJsonObject {
				{ QStringLiteral("crs"), QStringLiteral("EPSG:3857") },
			} },
			{ QStringLiteral("targetFrame"), QJsonObject {
				{ QStringLiteral("crs"), QStringLiteral("EPSG:3857") },
			} },
			{ QStringLiteral("operation"), QJsonObject {
				{ QStringLiteral("type"), QStringLiteral("affine2d") },
				{ QStringLiteral("unit"), QStringLiteral("crs") },
				{ QStringLiteral("xoff"), 1.0 },
				{ QStringLiteral("yoff"), 2.0 },
				{ QStringLiteral("s11"), 1.0 },
				{ QStringLiteral("s12"), 0.01 },
				{ QStringLiteral("s21"), -0.01 },
				{ QStringLiteral("s22"), 1.0 },
			} },
		};
		source.insert(QStringLiteral("registration"), registration);
	});
	auto const result = imagery::OicCatalogReader::read(bytes);
	QVERIFY(result.accepted());
	QCOMPARE(result.validSourceCount(), 1);
	QCOMPARE(result.supportedSourceCount(), 0);
	auto const& source = result.catalog.sources.first();
	QVERIFY(source.valid);
	QVERIFY(!source.supported);
	QVERIFY(!source.resolved_source);
	QCOMPARE(
		source.registration.kind,
		imagery::OicRegistrationKind::Affine2d
	);
	QVERIFY(source.unsupported_capabilities.contains(
		QStringLiteral("registration.affine2d.v1")
	));
	QVERIFY(hasDiagnostic(
		result, imagery::OicDiagnosticKind::UnsupportedSource,
		QStringLiteral("affine")
	));

	auto catalog = minimalCatalog();
	catalog.insert(QStringLiteral("resources"), QJsonObject {
		{ QStringLiteral("survey-grid"), QJsonObject {
			{ QStringLiteral("href"),
			  QStringLiteral("resources/survey-grid.tif") },
			{ QStringLiteral("mediaType"),
			  QStringLiteral("image/tiff; application=geodetic-grid") },
			{ QStringLiteral("sha256"), QString(64, QLatin1Char('a')) },
			{ QStringLiteral("size"), 123456 },
		} },
	});
	auto sources = catalog.value(QStringLiteral("sources")).toArray();
	auto grid_source = sources.first().toObject();
	grid_source.insert(QStringLiteral("registration"), QJsonObject {
		{ QStringLiteral("direction"),
		  QStringLiteral("source-to-corrected") },
		{ QStringLiteral("sourceFrame"), QJsonObject {
			{ QStringLiteral("crs"), QStringLiteral("EPSG:3857") },
		} },
		{ QStringLiteral("targetFrame"), QJsonObject {
			{ QStringLiteral("crs"), QStringLiteral("EPSG:3857") },
		} },
		{ QStringLiteral("operation"), QJsonObject {
			{ QStringLiteral("type"), QStringLiteral("gridShift") },
			{ QStringLiteral("resource"), QStringLiteral("survey-grid") },
			{ QStringLiteral("domain"), QStringLiteral("horizontal") },
			{ QStringLiteral("gridFrame"), QJsonObject {
				{ QStringLiteral("crs"), QStringLiteral("EPSG:3857") },
			} },
			{ QStringLiteral("interpolation"), QStringLiteral("bilinear") },
		} },
	});
	sources.replace(0, grid_source);
	catalog.insert(QStringLiteral("sources"), sources);
	auto const grid = imagery::OicCatalogReader::read(compact(catalog));
	QVERIFY(grid.accepted());
	QCOMPARE(grid.catalog.resources.size(), 1);
	QCOMPARE(
		grid.catalog.resources.first().sha256,
		QByteArray(64, 'a')
	);
	QVERIFY(grid.catalog.sources.first().valid);
	QVERIFY(!grid.catalog.sources.first().supported);
	QCOMPARE(
		grid.catalog.sources.first().registration.kind,
		imagery::OicRegistrationKind::GridShift
	);
	QVERIFY(grid.catalog.sources.first().unsupported_capabilities.contains(
		QStringLiteral("registration.grid-shift.v1")
	));
}

void OicCatalogTest::rejectsSingularAffine()
{
	auto bytes = changedSource([](QJsonObject& source) {
		source.insert(QStringLiteral("registration"), QJsonObject {
			{ QStringLiteral("direction"),
			  QStringLiteral("source-to-corrected") },
			{ QStringLiteral("sourceFrame"), QJsonObject {
				{ QStringLiteral("crs"), QStringLiteral("EPSG:3857") },
			} },
			{ QStringLiteral("targetFrame"), QJsonObject {
				{ QStringLiteral("crs"), QStringLiteral("EPSG:3857") },
			} },
			{ QStringLiteral("operation"), QJsonObject {
				{ QStringLiteral("type"), QStringLiteral("affine2d") },
				{ QStringLiteral("unit"), QStringLiteral("crs") },
				{ QStringLiteral("xoff"), 0.0 },
				{ QStringLiteral("yoff"), 0.0 },
				{ QStringLiteral("s11"), 1.0 },
				{ QStringLiteral("s12"), 2.0 },
				{ QStringLiteral("s21"), 2.0 },
				{ QStringLiteral("s22"), 4.0 },
			} },
		});
	});
	auto const result = imagery::OicCatalogReader::read(bytes);
	QVERIFY(!result.accepted());
	QCOMPARE(result.validSourceCount(), 0);
	QVERIFY(hasDiagnostic(
		result, imagery::OicDiagnosticKind::SourceError,
		QStringLiteral("singular-affine")
	));
}

void OicCatalogTest::retainsNonDyadicAndUnknownGridsAsUnsupported()
{
	auto const non_dyadic = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			source.remove(QStringLiteral("tileMatrixSetURI"));
			source.insert(
				QStringLiteral("tileMatrixSet"),
				inlineMatrixSet(false)
			);
		})
	);
	QVERIFY(non_dyadic.accepted());
	QVERIFY(non_dyadic.catalog.sources.first().valid);
	QVERIFY(!non_dyadic.catalog.sources.first().supported);
	QVERIFY(non_dyadic.catalog.sources.first()
		        .unsupported_capabilities.contains(
			        QStringLiteral("tile-matrix-set.nondyadic.v1")
		        ));

	auto const unknown = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			source.insert(
				QStringLiteral("tileMatrixSetURI"),
				QStringLiteral(
					"https://example.test/tile-matrix-sets/regional"
				)
			);
		})
	);
	QVERIFY(unknown.accepted());
	QVERIFY(unknown.catalog.sources.first().valid);
	QVERIFY(!unknown.catalog.sources.first().supported);
	QVERIFY(unknown.catalog.sources.first()
		        .unsupported_capabilities.contains(
			        QStringLiteral("tile-matrix-set.external.v1")
		        ));
}

void OicCatalogTest::retainsExternalMatrixLimitsUntilResolution()
{
	auto const result = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			source.insert(
				QStringLiteral("tileMatrixSetURI"),
				QStringLiteral(
					"https://example.test/tile-matrix-sets/regional"
				)
			);
			source.insert(QStringLiteral("tileMatrixLimits"), QJsonArray {
				QJsonObject {
					{ QStringLiteral("tileMatrix"),
					  QStringLiteral("regional-12") },
					{ QStringLiteral("minTileRow"), 2 },
					{ QStringLiteral("maxTileRow"), 7 },
					{ QStringLiteral("minTileCol"), 3 },
					{ QStringLiteral("maxTileCol"), 9 },
				},
			});
		})
	);
	QVERIFY(result.accepted());
	auto const& source = result.catalog.sources.first();
	QVERIFY(source.valid);
	QVERIFY(!source.supported);
	QCOMPARE(source.tile_limit_definitions.size(), 1);
	auto const& limit = source.tile_limit_definitions.first();
	QCOMPARE(limit.tile_matrix, QStringLiteral("regional-12"));
	QCOMPARE(limit.min_row, 2);
	QCOMPARE(limit.max_row, 7);
	QCOMPARE(limit.min_column, 3);
	QCOMPARE(limit.max_column, 9);
	QVERIFY(source.tile_limits.isEmpty());
	QCOMPARE(source.full_fingerprint.size(), 64);
	QCOMPARE(source.operational_fingerprint.size(), 64);
}

void OicCatalogTest::gatesUnsupportedAxisOrder()
{
	auto const result = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			auto matrix_set = inlineMatrixSet();
			matrix_set.insert(
				QStringLiteral("orderedAxes"),
				QJsonArray { QStringLiteral("N"), QStringLiteral("E") }
			);
			source.remove(QStringLiteral("tileMatrixSetURI"));
			source.insert(QStringLiteral("tileMatrixSet"), matrix_set);
		})
	);
	QVERIFY(result.accepted());
	auto const& source = result.catalog.sources.first();
	QVERIFY(source.valid);
	QVERIFY(!source.supported);
	QVERIFY(source.unsupported_capabilities.contains(
		QStringLiteral("tile-matrix-set.axis-order.v1")
	));
	QVERIFY(hasDiagnostic(
		result, imagery::OicDiagnosticKind::UnsupportedSource,
		QStringLiteral("axis")
	));
}

void OicCatalogTest::recognizesOnlyExactWebMercatorQuadUris()
{
	auto const https = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			source.insert(
				QStringLiteral("tileMatrixSetURI"),
				QStringLiteral(
					"HTTPS://WWW.OPENGIS.NET:443/def/tilematrixset/"
					"OGC/1.0/WebMercatorQuad"
				)
			);
		})
	);
	QVERIFY(https.accepted());
	QVERIFY(https.catalog.sources.first().supported);

	for (auto const& uri : {
		QStringLiteral(
			"https://www.opengis.net:8443/def/tilematrixset/"
			"OGC/1.0/WebMercatorQuad"
		),
		QStringLiteral(
			"https://www.opengis.net/def/tilematrixset/"
			"OGC/1.0/WebMercatorQuad?profile=other"
		),
		QStringLiteral(
			"https://www.opengis.net/def/tilematrixset/"
			"OGC/1.0/WebMercatorQuad/extra"
		),
	})
	{
		auto const result = imagery::OicCatalogReader::read(
			changedSource([&uri](QJsonObject& source) {
				source.insert(QStringLiteral("tileMatrixSetURI"), uri);
			})
		);
		QVERIFY2(result.accepted(), qPrintable(uri));
		QVERIFY(result.catalog.sources.first().valid);
		QVERIFY(!result.catalog.sources.first().supported);
		QVERIFY(result.catalog.sources.first()
			        .unsupported_capabilities.contains(
				        QStringLiteral("tile-matrix-set.external.v1")
			        ));
	}
}

void OicCatalogTest::validatesFrameIdsEvenWhenNotRetained()
{
	auto const result = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			auto registration = translationRegistration();
			auto source_frame =
				registration.value(QStringLiteral("sourceFrame")).toObject();
			source_frame.insert(
				QStringLiteral("id"), QStringLiteral("invalid frame id")
			);
			registration.insert(
				QStringLiteral("sourceFrame"), source_frame
			);
			source.insert(
				QStringLiteral("registration"), registration
			);
		})
	);
	QVERIFY(!result.accepted());
	QVERIFY(hasDiagnostic(
		result, imagery::OicDiagnosticKind::SourceError,
		QStringLiteral("$.sources[0].registration.sourceFrame.id")
	));
}

void OicCatalogTest::rejectsNonfiniteDerivedMatrixExtents()
{
	auto matrix_set = imagery::TileMatrixSet::webMercatorQuad();
	matrix_set.matrices[0].cell_size =
		std::numeric_limits<double>::max() / 128.0;
	QString error;
	QVERIFY(!matrix_set.validateDyadicTopLeft(&error));
	QVERIFY(error.contains(QStringLiteral("extent")));
}

void OicCatalogTest::rejectsUnsafeOrAmbiguousTemplates_data()
{
	QTest::addColumn<QByteArray>("input");
	QTest::newRow("file-url")
		<< changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("tiles"), QJsonArray {
				QStringLiteral("file:///tmp/{z}/{x}/{y}.png"),
			});
		});
	QTest::newRow("userinfo")
		<< changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("tiles"), QJsonArray {
				QStringLiteral(
					"https://user:secret@example.test/{z}/{x}/{y}.png"
				),
			});
		});
	QTest::newRow("empty-userinfo")
		<< changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("tiles"), QJsonArray {
				QStringLiteral(
					"https://@example.test/{z}/{x}/{y}.png"
				),
			});
		});
	QTest::newRow("missing-placeholder")
		<< changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("tiles"), QJsonArray {
				QStringLiteral("https://example.test/{z}/{x}/tile.png"),
			});
		});
	QTest::newRow("unknown-placeholder")
		<< changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("tiles"), QJsonArray {
				QStringLiteral(
					"https://example.test/{z}/{x}/{y}/{s}.png"
				),
			});
		});
	QTest::newRow("placeholder-in-authority")
		<< changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("tiles"), QJsonArray {
				QStringLiteral(
					"https://{z}.example.test/tiles/{x}/{y}.png"
				),
			});
		});
	QTest::newRow("fragment")
		<< changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("tiles"), QJsonArray {
				QStringLiteral(
					"https://example.test/{z}/{x}/{y}.png#fragment"
				),
			});
		});
	QTest::newRow("raw-space")
		<< changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("tiles"), QJsonArray {
				QStringLiteral(
					"https://example.test/raw space/{z}/{x}/{y}.png"
				),
			});
		});
	QTest::newRow("duplicate")
		<< changedSource([](QJsonObject& source) {
			auto const tile =
				QStringLiteral("https://example.test/{z}/{x}/{y}.png");
			source.insert(
				QStringLiteral("tiles"), QJsonArray { tile, tile }
			);
		});
}

void OicCatalogTest::rejectsUnsafeOrAmbiguousTemplates()
{
	QFETCH(QByteArray, input);
	auto const result = imagery::OicCatalogReader::read(input);
	QVERIFY(!result.accepted());
	QVERIFY(!result.hasCatalogErrors());
	QVERIFY(hasDiagnostic(
		result, imagery::OicDiagnosticKind::SourceError
	));
}

void OicCatalogTest::lexicalGuardrails_data()
{
	QTest::addColumn<QByteArray>("input");
	QTest::newRow("duplicate-member")
		<< QByteArray(
			R"({"format":"org.openorienteering.imagery-catalog","format":"org.openorienteering.imagery-catalog"})"
		);
	QTest::newRow("escaped-duplicate-member")
		<< QByteArray(R"({"id":"one","\u0069d":"two"})");
	QTest::newRow("invalid-utf8")
		<< QByteArray("{\"x\":\"")
		     + QByteArray(1, char(0xc0)) + QByteArray("\"}");
	QTest::newRow("unpaired-surrogate")
		<< QByteArray(R"({"x":"\ud800"})");
	QTest::newRow("nonfinite-number")
		<< QByteArray(R"({"x":1e400})");
	QTest::newRow("negative-zero")
		<< QByteArray(R"({"x":-0.0})");
	QTest::newRow("inexact-integer")
		<< QByteArray(R"({"x":9007199254740992})");
	QByteArray nested;
	for (int index = 0;
	     index < imagery::OicCatalogReader::maximum_nesting_depth + 1;
	     ++index)
	{
		nested += '[';
	}
	nested += '0';
	for (int index = 0;
	     index < imagery::OicCatalogReader::maximum_nesting_depth + 1;
	     ++index)
	{
		nested += ']';
	}
	QTest::newRow("nesting") << nested;
}

void OicCatalogTest::lexicalGuardrails()
{
	QFETCH(QByteArray, input);
	auto const result = imagery::OicCatalogReader::read(input);
	QVERIFY(!result.accepted());
	QVERIFY(result.hasCatalogErrors());
	QVERIFY(hasDiagnostic(
		result, imagery::OicDiagnosticKind::CatalogError,
		QStringLiteral("json-preflight")
	));
}

void OicCatalogTest::documentSizeLimit()
{
	QByteArray oversized(
		imagery::OicCatalogReader::maximum_document_size + 1, ' '
	);
	auto const result = imagery::OicCatalogReader::read(oversized);
	QVERIFY(!result.accepted());
	QVERIFY(result.hasCatalogErrors());
	QVERIFY(hasDiagnostic(
		result, imagery::OicDiagnosticKind::CatalogError,
		QStringLiteral("document-size")
	));
}


void OicCatalogTest::boundsResourceProcessing()
{
	auto object = minimalCatalog();
	QJsonObject resources;
	for (int index = 0;
	     index < imagery::OicCatalogReader::maximum_resources + 1;
	     ++index)
	{
		resources.insert(
			QStringLiteral("resource-%1").arg(
				index,
				4,
				10,
				QLatin1Char('0')),
			QJsonObject {
				{ QStringLiteral("href"),
				  QStringLiteral("resources/item.bin") },
				{ QStringLiteral("mediaType"),
				  QStringLiteral("application/octet-stream") },
				{ QStringLiteral("sha256"),
				  QString(64, QLatin1Char('a')) },
				{ QStringLiteral("size"), 1 },
			});
	}
	object.insert(QStringLiteral("resources"), resources);
	auto const result =
		imagery::OicCatalogReader::read(compact(object));
	QVERIFY(!result.accepted());
	QVERIFY(hasDiagnostic(
		result,
		imagery::OicDiagnosticKind::CatalogError,
		QStringLiteral("resource-limit")));
	QCOMPARE(
		result.catalog.resources.size(),
		imagery::OicCatalogReader::maximum_resources);
}


void OicCatalogTest::invalidSourceDoesNotDiscardValidSource()
{
	auto catalog = minimalCatalog();
	auto sources = catalog.value(QStringLiteral("sources")).toArray();
	auto invalid = minimalSource();
	invalid.insert(QStringLiteral("id"), QStringLiteral("invalid"));
	invalid.insert(QStringLiteral("tiles"), QJsonArray {
		QStringLiteral("file:///invalid/{z}/{x}/{y}.png"),
	});
	sources.push_back(invalid);
	catalog.insert(QStringLiteral("sources"), sources);

	auto const result = imagery::OicCatalogReader::read(compact(catalog));
	QVERIFY(result.accepted());
	QCOMPARE(result.catalog.sources.size(), 2);
	QCOMPARE(result.validSourceCount(), 1);
	QCOMPARE(result.supportedSourceCount(), 1);
	QVERIFY(!result.catalog.sources.at(1).valid);
	QVERIFY(hasDiagnostic(
		result, imagery::OicDiagnosticKind::SourceError,
		QStringLiteral("$.sources[1].tiles[0]")
	));
}

void OicCatalogTest::capabilitiesAreGatedAtTheCorrectLevel()
{
	auto const source_capability = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("requires"), QJsonArray {
				QStringLiteral("org.example.future-renderer"),
			});
		})
	);
	QVERIFY(source_capability.accepted());
	QVERIFY(source_capability.catalog.sources.first().valid);
	QVERIFY(!source_capability.catalog.sources.first().supported);
	QVERIFY(hasDiagnostic(
		source_capability,
		imagery::OicDiagnosticKind::UnsupportedSource,
		QStringLiteral("future-renderer")
	));

	auto const catalog_capability = imagery::OicCatalogReader::read(
		changedCatalog([](QJsonObject& catalog) {
			catalog.insert(QStringLiteral("requires"), QJsonArray {
				QStringLiteral("org.example.future-catalog"),
			});
		})
	);
	QVERIFY(!catalog_capability.accepted());
	QVERIFY(catalog_capability.hasCatalogErrors());
	QVERIFY(catalog_capability.resolvedSources().isEmpty());
	QVERIFY(!catalog_capability.catalog.sources.first().resolved_source);
	QVERIFY(hasDiagnostic(
		catalog_capability,
		imagery::OicDiagnosticKind::CatalogError,
		QStringLiteral("unsupported-capability")
	));
}

void OicCatalogTest::validatesTileLimitsAndDiagnostics()
{
	auto const result = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			source.remove(QStringLiteral("tileMatrixSetURI"));
			source.insert(
				QStringLiteral("tileMatrixSet"), inlineMatrixSet()
			);
			source.insert(
				QStringLiteral("minTileMatrix"), QStringLiteral("0")
			);
			source.insert(
				QStringLiteral("maxTileMatrix"), QStringLiteral("2")
			);
			source.insert(QStringLiteral("tileMatrixLimits"), QJsonArray {
				QJsonObject {
					{ QStringLiteral("tileMatrix"), QStringLiteral("2") },
					{ QStringLiteral("minTileRow"), 0 },
					{ QStringLiteral("maxTileRow"), 3 },
					{ QStringLiteral("minTileCol"), 0 },
					{ QStringLiteral("maxTileCol"), 4 },
				},
			});
		})
	);
	QVERIFY(!result.accepted());
	QVERIFY(hasDiagnostic(
		result, imagery::OicDiagnosticKind::SourceError,
		QStringLiteral("limit-bounds")
	));
	auto found_path = false;
	for (auto const& diagnostic : result.diagnostics)
	{
		if (diagnostic.code == QLatin1String("limit-bounds"))
		{
			QCOMPARE(
				diagnostic.path,
				QStringLiteral("$.sources[0].tileMatrixLimits[0]")
			);
			QCOMPARE(diagnostic.source_index, 0);
			QVERIFY(diagnostic.displayText().contains(diagnostic.path));
			found_path = true;
		}
	}
	QVERIFY(found_path);
}

void OicCatalogTest::fingerprintsAreDeterministic()
{
	auto const object = minimalCatalog();
	auto const compact_bytes =
		QJsonDocument(object).toJson(QJsonDocument::Compact);
	auto const indented_bytes =
		QJsonDocument(object).toJson(QJsonDocument::Indented);
	auto const first = imagery::OicCatalogReader::read(compact_bytes);
	auto const second = imagery::OicCatalogReader::read(indented_bytes);
	QVERIFY(first.accepted());
	QVERIFY(second.accepted());
	QCOMPARE(
		first.catalog.sources.first().full_fingerprint,
		second.catalog.sources.first().full_fingerprint
	);
	QCOMPARE(
		first.catalog.sources.first().operational_fingerprint,
		second.catalog.sources.first().operational_fingerprint
	);
	QVERIFY(first.catalog.document_sha256 != second.catalog.document_sha256);

	auto const alias = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			source.insert(QStringLiteral("tiles"), QJsonArray {
				QStringLiteral(
					"HTTPS://TILES.EXAMPLE.TEST:443/aerial/{z}/{x}/{y}.png"
				),
			});
		})
	);
	QVERIFY(alias.accepted());
	QCOMPARE(
		first.catalog.sources.first().full_fingerprint,
		alias.catalog.sources.first().full_fingerprint
	);
	QCOMPARE(
		first.catalog.sources.first().operational_fingerprint,
		alias.catalog.sources.first().operational_fingerprint
	);

	auto const renamed = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			source.insert(
				QStringLiteral("name"), QStringLiteral("Renamed imagery")
			);
		})
	);
	QVERIFY(renamed.accepted());
	QVERIFY(
		first.catalog.sources.first().full_fingerprint
		!= renamed.catalog.sources.first().full_fingerprint
	);
	QCOMPARE(
		first.catalog.sources.first().operational_fingerprint,
		renamed.catalog.sources.first().operational_fingerprint
	);
}

void OicCatalogTest::fullFingerprintRetainsMatrixSetDescription()
{
	auto baseline = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			auto matrix_set = inlineMatrixSet();
			matrix_set.insert(
				QStringLiteral("title"),
				QStringLiteral("Published grid title")
			);
			source.remove(QStringLiteral("tileMatrixSetURI"));
			source.insert(QStringLiteral("tileMatrixSet"), matrix_set);
		})
	);
	auto renamed = imagery::OicCatalogReader::read(
		changedSource([](QJsonObject& source) {
			auto matrix_set = inlineMatrixSet();
			matrix_set.insert(
				QStringLiteral("title"),
				QStringLiteral("Revised published grid title")
			);
			source.remove(QStringLiteral("tileMatrixSetURI"));
			source.insert(QStringLiteral("tileMatrixSet"), matrix_set);
		})
	);
	QVERIFY(baseline.accepted());
	QVERIFY(renamed.accepted());
	QVERIFY(
		baseline.catalog.sources.first().full_fingerprint
		!= renamed.catalog.sources.first().full_fingerprint
	);
	QCOMPARE(
		baseline.catalog.sources.first().operational_fingerprint,
		renamed.catalog.sources.first().operational_fingerprint
	);
}

QTEST_APPLESS_MAIN(OicCatalogTest)
