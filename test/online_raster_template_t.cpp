/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include <algorithm>
#include <atomic>
#include <memory>

#include <QBuffer>
#include <QImage>
#include <QLineF>
#include <QPainter>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QtTest>

#include "core/georeferencing.h"
#include "core/map.h"
#include "core/map_color.h"
#include "core/objects/object.h"
#include "core/symbols/line_symbol.h"
#include "global.h"
#include "imagery/imagery_source_snapshot.h"
#include "templates/online_raster_template.h"
#include "util/util.h"

void initOnlineRasterTemplateTestResources()
{
	Q_INIT_RESOURCE(resources);
}

namespace OpenOrienteering {

namespace {

imagery::ResolvedImagerySource sourceFixture()
{
	auto matrix_set = imagery::TileMatrixSet::webMercatorQuad();
	matrix_set.matrices.resize(3);
	for (auto& matrix : matrix_set.matrices)
	{
		matrix.cell_size *= 64;
		matrix.tile_size = { 4, 4 };
	}

	imagery::ResolvedImagerySource source;
	source.metadata.id = QStringLiteral("org.example.online-test");
	source.metadata.name = QStringLiteral("Online test imagery");
	source.metadata.category = imagery::ImageryCategory::Aerial;
	source.notices.attribution_text = QStringLiteral("Example imagery");
	source.tile_urls = {
		{ QStringLiteral("https://tiles.example.test/{z}/{x}/{y}.png") },
	};
	source.media_type = QStringLiteral("image/png");
	source.tile_matrix_set = std::move(matrix_set);
	source.min_zoom = 0;
	source.max_zoom = 2;
	return source;
}

imagery::ImagerySourceSnapshot
snapshotFixture(imagery::ResolvedImagerySource source = sourceFixture())
{
	QString error;
	auto snapshot = imagery::ImagerySourceSnapshotCodec::encode(source, &error);
	Q_ASSERT_X(snapshot, Q_FUNC_INFO, qPrintable(error));
	return std::move(*snapshot);
}

imagery::ResolvedImagerySource geographicSourceFixture()
{
	imagery::TileMatrixSet matrix_set;
	matrix_set.id = QStringLiteral("WorldCRS84Quad");
	matrix_set.crs = QStringLiteral("EPSG:4326");
	for (int zoom = 0; zoom <= 6; ++zoom)
	{
		auto const dimension = qint64(1) << zoom;
		matrix_set.matrices.push_back({
			QString::number(zoom),
			zoom,
			45.0 / dimension,
			QPointF(-180, 90),
			QSize(4, 4),
			2 * dimension,
			dimension,
		});
	}
	auto source = sourceFixture();
	source.tile_matrix_set = std::move(matrix_set);
	source.min_zoom = 0;
	source.max_zoom = 6;
	return source;
}

void georeferenceMap(Map& map)
{
	Georeferencing georef;
	georef.setScaleDenominator(1000);
	auto const valid =
		georef.setProjectedCRS(QStringLiteral("EPSG:3857"), QStringLiteral("EPSG:3857"));
	Q_ASSERT(valid);
	georef.setProjectedRefPoint({ 0, 0 }, false, false);
	map.setGeoreferencing(georef);
}

void georeferenceUtmMap(Map& map)
{
	Georeferencing georef;
	georef.setScaleDenominator(1000);
	auto const valid =
		georef.setProjectedCRS(QStringLiteral("EPSG:32610"), QStringLiteral("EPSG:32610"));
	Q_ASSERT(valid);
	georef.setGeographicRefPoint({ 47.5, -122.5 }, false, false);
	map.setGeoreferencing(georef);
}

QImage paddedImage(QSize core_size, QColor color)
{
	QImage image(core_size.width() + 2, core_size.height() + 2, QImage::Format_RGBA8888);
	image.fill(color);
	return image;
}

QByteArray encodedPng(QSize size, QColor color)
{
	QImage image(size, QImage::Format_RGBA8888);
	image.fill(color);
	QByteArray bytes;
	QBuffer buffer(&bytes);
	if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
		return {};
	return bytes;
}

} // namespace

class OnlineRasterTemplateTest : public QObject
{
	Q_OBJECT

	static QRectF mapRectForWindow(OnlineRasterTemplate& source,
								   const OnlineRasterTemplate::TileWindow& window)
	{
		QRectF result;
		for (auto const& key : {
				 OnlineRasterTileKey{ window.zoom, window.min_column, window.min_row },
				 OnlineRasterTileKey{ window.zoom, window.max_column, window.max_row },
			 })
		{
			auto const bounds = source.tileBounds(key);
			for (auto const& point : {
					 QPointF(bounds.west, bounds.north),
					 QPointF(bounds.east, bounds.south),
				 })
			{
				auto mapped = source.nominalSourceToMap(point);
				Q_ASSERT(mapped);
				rectIncludeSafe(result, *mapped);
			}
		}
		return result;
	}

  private slots:
	void initTestCase()
	{
		initOnlineRasterTemplateTestResources();
		doStaticInitializations();
	}

	void snapshotConfigurationRoundTripsExactly()
	{
		Map map;
		auto snapshot = snapshotFixture();
		OnlineRasterTemplate original(snapshot, &map);
		original.setDisplayName(QStringLiteral("Custom display name"));

		QByteArray xml_bytes;
		QXmlStreamWriter writer(&xml_bytes);
		writer.writeStartDocument();
		original.saveTemplateConfiguration(writer, true);
		writer.writeEndDocument();

		QXmlStreamReader reader(xml_bytes);
		QVERIFY(reader.readNextStartElement());
		bool open = false;
		auto loaded = Template::loadTemplateConfiguration(reader, map, open);
		QVERIFY2(loaded, qPrintable(reader.errorString()));
		QVERIFY(open);
		auto* online = dynamic_cast<OnlineRasterTemplate*>(loaded.get());
		QVERIFY(online);
		QVERIFY2(
			online->sourceSnapshot(),
			qPrintable(online->snapshot_error_));
		QCOMPARE(online->sourceSnapshot()->canonical_json, snapshot.canonical_json);
		QCOMPARE(online->sourceSnapshot()->sha256, snapshot.sha256);
		QCOMPARE(online->sourceSnapshot()->source, snapshot.source);
		QCOMPARE(
			online->getTemplateFilename(),
			QStringLiteral("Custom display name"));
		QCOMPARE(online->getTemplateType(), "OnlineRasterTemplate");
		QVERIFY(online->fileExists());
		QCOMPARE(online->tryToFindTemplateFile({}), Template::FoundByAbsPath);
	}

	void badEmbeddedSnapshotIsRetainedButCannotLoad()
	{
		Map map;
		auto snapshot = snapshotFixture();
		OnlineRasterTemplate original(snapshot, &map);

		QByteArray xml_bytes;
		QXmlStreamWriter writer(&xml_bytes);
		original.saveTemplateConfiguration(writer, true);
		auto const needle = snapshot.canonical_json.toBase64();
		auto damaged = needle;
		damaged[damaged.size() / 2] = damaged.at(damaged.size() / 2) == 'A' ? 'B' : 'A';
		QVERIFY(xml_bytes.contains(needle));
		xml_bytes.replace(needle, damaged);

		QXmlStreamReader reader(xml_bytes);
		QVERIFY(reader.readNextStartElement());
		bool open = false;
		auto loaded = Template::loadTemplateConfiguration(reader, map, open);
		auto* online = dynamic_cast<OnlineRasterTemplate*>(loaded.get());
		QVERIFY(online);
		QVERIFY(!online->sourceSnapshot());
		QVERIFY(!online->stored_snapshot_json_.isEmpty());
		online->setTemplateState(Template::Unloaded);
		QVERIFY(!online->loadTemplateFile());
		QCOMPARE(online->getTemplateState(), Template::Invalid);
		QVERIFY(online->errorString().contains(QStringLiteral("checksum"), Qt::CaseInsensitive));

		QByteArray resaved;
		QXmlStreamWriter resave_writer(&resaved);
		online->saveTemplateConfiguration(resave_writer, true);
		QVERIFY(resaved.contains(damaged));
	}

	void unknownSnapshotVersionRoundTripsRawPayload()
	{
		Map map;
		auto snapshot = snapshotFixture();
		OnlineRasterTemplate original(snapshot, &map);
		QByteArray xml_bytes;
		QXmlStreamWriter writer(&xml_bytes);
		original.saveTemplateConfiguration(writer, true);
		QVERIFY(xml_bytes.contains("snapshot_version=\"1\""));
		xml_bytes.replace("snapshot_version=\"1\"", "snapshot_version=\"99\"");

		QXmlStreamReader reader(xml_bytes);
		QVERIFY(reader.readNextStartElement());
		bool open = false;
		auto loaded = Template::loadTemplateConfiguration(reader, map, open);
		auto* online = dynamic_cast<OnlineRasterTemplate*>(loaded.get());
		QVERIFY(online);
		QVERIFY(!online->sourceSnapshot());
		QVERIFY(online->snapshot_error_.contains(QStringLiteral("99")));

		QByteArray resaved;
		QXmlStreamWriter resave_writer(&resaved);
		online->saveTemplateConfiguration(resave_writer, true);
		QVERIFY(resaved.contains("snapshot_version=\"99\""));
		QVERIFY(resaved.contains(snapshot.canonical_json.toBase64()));
	}

	void invalidBase64RoundTripsRawPayload()
	{
		Map map;
		auto snapshot = snapshotFixture();
		OnlineRasterTemplate original(snapshot, &map);
		QByteArray xml_bytes;
		QXmlStreamWriter writer(&xml_bytes);
		original.saveTemplateConfiguration(writer, true);
		auto const valid_payload = snapshot.canonical_json.toBase64();
		QVERIFY(xml_bytes.contains(valid_payload));
		xml_bytes.replace(valid_payload, "%%%not-base64%%%");

		QXmlStreamReader reader(xml_bytes);
		QVERIFY(reader.readNextStartElement());
		bool open = false;
		auto loaded = Template::loadTemplateConfiguration(reader, map, open);
		auto* online = dynamic_cast<OnlineRasterTemplate*>(loaded.get());
		QVERIFY(online);
		QVERIFY(!online->sourceSnapshot());
		QVERIFY(online->snapshot_error_.contains(QStringLiteral("base64"), Qt::CaseInsensitive));

		QByteArray resaved;
		QXmlStreamWriter resave_writer(&resaved);
		online->saveTemplateConfiguration(resave_writer, true);
		QVERIFY(resaved.contains("%%%not-base64%%%"));
	}

	void duplicatePayloadsAreRetainedAndRejected()
	{
		Map map;
		auto snapshot = snapshotFixture();
		OnlineRasterTemplate original(snapshot, &map);
		QByteArray xml_bytes;
		QXmlStreamWriter writer(&xml_bytes);
		original.saveTemplateConfiguration(writer, true);
		auto const payload = snapshot.canonical_json.toBase64();
		QByteArray duplicate("<snapshot_json encoding=\"base64\">");
		duplicate += payload;
		duplicate += "</snapshot_json>";
		auto replacement = duplicate;
		replacement += "</online_source>";
		QVERIFY(xml_bytes.contains("</online_source>"));
		xml_bytes.replace("</online_source>", replacement);

		QXmlStreamReader reader(xml_bytes);
		QVERIFY(reader.readNextStartElement());
		bool open = false;
		auto loaded = Template::loadTemplateConfiguration(reader, map, open);
		auto* online = dynamic_cast<OnlineRasterTemplate*>(loaded.get());
		QVERIFY(online);
		QVERIFY(!online->sourceSnapshot());
		QCOMPARE(online->stored_snapshot_payloads_.size(), 2);
		QVERIFY(online->snapshot_error_.contains(QStringLiteral("multiple"), Qt::CaseInsensitive));

		QByteArray resaved;
		QXmlStreamWriter resave_writer(&resaved);
		online->saveTemplateConfiguration(resave_writer, true);
		QCOMPARE(resaved.count("<snapshot_json"), 2);
	}

	void oversizedEmbeddedPayloadIsDiscardedBeforeDecode()
	{
		Map map;
		auto snapshot = snapshotFixture();
		OnlineRasterTemplate original(snapshot, &map);
		QByteArray xml_bytes;
		QXmlStreamWriter writer(&xml_bytes);
		original.saveTemplateConfiguration(writer, true);
		auto const valid_payload = snapshot.canonical_json.toBase64();
		auto const oversized = QByteArray(
			imagery::ImagerySourceSnapshotCodec::maximum_base64_size + 1,
			'A');
		QVERIFY(xml_bytes.contains(valid_payload));
		xml_bytes.replace(valid_payload, oversized);

		QXmlStreamReader reader(xml_bytes);
		QVERIFY(reader.readNextStartElement());
		bool open = false;
		auto loaded = Template::loadTemplateConfiguration(reader, map, open);
		auto* online = dynamic_cast<OnlineRasterTemplate*>(loaded.get());
		QVERIFY(online);
		QVERIFY(!online->sourceSnapshot());
		QVERIFY(online->stored_snapshot_payloads_.isEmpty());
		QVERIFY(online->stored_snapshot_json_.isEmpty());
		QVERIFY(online->snapshot_error_.contains(
			QStringLiteral("size limit"),
			Qt::CaseInsensitive));

		QByteArray resaved;
		QXmlStreamWriter resave_writer(&resaved);
		online->saveTemplateConfiguration(resave_writer, true);
		QVERIFY(!resaved.contains(oversized.left(1024)));
		QVERIFY(resaved.size() < 4096);
	}

	void excessiveEmbeddedPayloadCountIsBounded()
	{
		Map map;
		auto snapshot = snapshotFixture();
		OnlineRasterTemplate original(snapshot, &map);
		QByteArray xml_bytes;
		QXmlStreamWriter writer(&xml_bytes);
		original.saveTemplateConfiguration(writer, true);
		auto const payload = snapshot.canonical_json.toBase64();
		QByteArray extra("<snapshot_json encoding=\"base64\">");
		extra += payload;
		extra += "</snapshot_json>";
		QByteArray replacement = extra;
		replacement += extra;
		replacement += "</online_source>";
		QVERIFY(xml_bytes.contains("</online_source>"));
		xml_bytes.replace("</online_source>", replacement);

		QXmlStreamReader reader(xml_bytes);
		QVERIFY(reader.readNextStartElement());
		bool open = false;
		auto loaded = Template::loadTemplateConfiguration(reader, map, open);
		auto* online = dynamic_cast<OnlineRasterTemplate*>(loaded.get());
		QVERIFY(online);
		QVERIFY(!online->sourceSnapshot());
		QCOMPARE(online->stored_snapshot_payloads_.size(), 2);
		QVERIFY(online->snapshot_error_.contains(
			QStringLiteral("too many"),
			Qt::CaseInsensitive));

		QByteArray resaved;
		QXmlStreamWriter resave_writer(&resaved);
		online->saveTemplateConfiguration(resave_writer, true);
		QCOMPARE(resaved.count("<snapshot_json"), 2);
	}

	void translationRegistrationRoundTrips()
	{
		Map map;
		georeferenceMap(map);
		auto source = sourceFixture();
		source.registration = imagery::TranslationRegistration{
			QStringLiteral("EPSG:3857"),
			QStringLiteral("EPSG:3857"),
			QStringLiteral("test-frame"),
			125.5,
			-44.25,
			{},
		};
		OnlineRasterTemplate online(snapshotFixture(source), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));

		QPointF nominal(12345.25, -2345.75);
		auto map_point = online.nominalSourceToMap(nominal);
		QVERIFY(map_point);
		auto round_trip = online.mapToNominalSource(*map_point);
		QVERIFY(round_trip);
		QVERIFY(QLineF(*round_trip, nominal).length() < 1.0e-6);
	}

	void windowHonorsMatrixLimits()
	{
		Map map;
		georeferenceMap(map);
		auto source = sourceFixture();
		source.tile_limits = { { 2, 1, 2, 1, 2 } };
		OnlineRasterTemplate online(snapshotFixture(source), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));

		auto const full = mapRectForWindow(online, { 2, 0, 3, 0, 3 });
		QCOMPARE(online.tileWindowForMapRect(full, 2),
				 (OnlineRasterTemplate::TileWindow{ 2, 1, 2, 1, 2 }));
		QVERIFY(!online.tileAllowed({ 2, 0, 1 }));
		QVERIFY(online.tileAllowed({ 2, 1, 1 }));
	}

	void screenExtentTracksMapContentWithMargin()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		QVERIFY(online.loadTemplateFile());
		auto const source_bounds = online.sourceMapBounds().normalized();
		QVERIFY(source_bounds.isValid());
		QVERIFY(!source_bounds.isEmpty());

		auto const content_seed = QRectF(-1000, -500, 2000, 1000);
		auto* color = new MapColor;
		color->setRgb(MapColorRgb(Qt::black));
		map.addColor(color, 0);
		auto* symbol = new LineSymbol;
		symbol->setColor(color);
		symbol->setLineWidth(1);
		map.addSymbol(symbol, 0);
		auto* object = new PathObject(symbol, {
			MapCoord(content_seed.topLeft()),
			MapCoord(content_seed.topRight()),
			MapCoord(content_seed.bottomRight()),
			MapCoord(content_seed.bottomLeft()),
		}, &map);
		map.addObject(object);

		auto const content_bounds = map.calculateExtent(false, false).normalized();
		auto const expected = source_bounds.intersected(content_bounds.adjusted(
			-content_bounds.width() * 0.2,
			-content_bounds.height() * 0.2,
			content_bounds.width() * 0.2,
			content_bounds.height() * 0.2));
		QCOMPARE(online.calculateTemplateBoundingBox(), expected);
		QVERIFY(!online.screen_map_bounds_dirty_);
		map.setObjectAreaDirty(content_bounds);
		QVERIFY(online.screen_map_bounds_dirty_);
		QCOMPARE(online.calculateTemplateBoundingBox(), expected);
		QVERIFY(!online.screen_map_bounds_dirty_);
		QVERIFY(expected.width() < source_bounds.width() * 0.2);
		QVERIFY(expected.height() < source_bounds.height() * 0.2);

		auto const outside = QRectF(
			source_bounds.topLeft(),
			QSizeF(source_bounds.width() * 0.05,
			       source_bounds.height() * 0.05));
		QVector<RasterTemplateTile> screen_tiles;
		online.collectRasterTiles(outside, 1, true, screen_tiles);
		QVERIFY(screen_tiles.isEmpty());
	}

	void fallbackCropIsExactAndPrintDisallowsIt()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));

		online.insertTile({ 1, 1, 1 }, { paddedImage({ 4, 4 }, Qt::red), true });
		OnlineRasterTileKey cached;
		QRectF source_rect;
		QVERIFY(online.bestCachedTile({ 2, 3, 2 }, &cached, &source_rect));
		QCOMPARE(cached, (OnlineRasterTileKey{ 1, 1, 1 }));
		QCOMPARE(source_rect, QRectF(3, 1, 2, 2));

		bool transparent = false;
		bool missing = false;
		bool pixels = false;
		auto screen = online.visualTiles({ 2, 3, 3, 2, 2 }, true, &transparent, &missing, &pixels);
		QCOMPARE(screen.size(), 1);
		QVERIFY(screen.front().provisional);
		QVERIFY(!missing);
		QVERIFY(pixels);

		auto print = online.visualTiles({ 2, 3, 3, 2, 2 }, false, &transparent, &missing, &pixels);
		QCOMPARE(print.size(), 1);
		QVERIFY(!print.front().tile);
		QVERIFY(missing);
		QVERIFY(!pixels);
	}

	void zoomOutReprojectsCachedDescendantsAsProvisionalCoverage()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));

		for (qint64 row = 0; row < 2; ++row)
		{
			for (qint64 column = 0; column < 2; ++column)
			{
				online.insertTile(
					{ 2, column, row },
					{ paddedImage(
						  { 4, 4 },
						  QColor::fromHsv(int(90 * (2 * row + column)), 255, 255)),
					  true, false });
			}
		}

		bool transparent = false;
		bool missing = false;
		bool pixels = false;
		auto const window = OnlineRasterTemplate::TileWindow { 1, 0, 0, 0, 0 };
		auto const visuals = online.visualTiles(
			window, true, &transparent, &missing, &pixels);
		QCOMPARE(visuals.size(), 4);
		QVERIFY(!transparent);
		QVERIFY(!missing);
		QVERIFY(pixels);
		for (auto const& visual : visuals)
		{
			QCOMPARE(visual.requested, (OnlineRasterTileKey { 1, 0, 0 }));
			QCOMPARE(visual.cached.zoom, 2);
			QVERIFY(visual.provisional);
			QVERIFY(visual.tile);
			QVERIFY(visual.target_rect.isValid());
			QCOMPARE(visual.target_rect.size(), QSizeF(0.5, 0.5));
		}

		online.wanted_window_ = window;
		QVector<RasterTemplateTile> tiles;
		online.collectRasterTiles(
			mapRectForWindow(online, window), 1.0e-5, true, tiles);
		QVERIFY(tiles.size() >= 4);
		QVERIFY(std::ranges::all_of(tiles, [](auto const& tile) {
			return !tile.missing && tile.provisional && tile.has_image_to_map;
		}));
	}

	void partialCachedDescendantsPreservePixelsAndReportMissingCoverage()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
		online.insertTile(
			{ 2, 0, 0 }, { paddedImage({ 4, 4 }, Qt::red), true, false });

		bool transparent = false;
		bool missing = false;
		bool pixels = false;
		auto const visuals = online.visualTiles(
			{ 1, 0, 0, 0, 0 }, true, &transparent, &missing, &pixels);
		QVERIFY(!transparent);
		QVERIFY(missing);
		QVERIFY(pixels);
		QCOMPARE(std::ranges::count_if(visuals, [](auto const& visual) {
			return visual.tile != nullptr;
		}), 1);
		QCOMPARE(std::ranges::count_if(visuals, [](auto const& visual) {
			return visual.tile == nullptr;
		}), 1);
	}

	void transparentCachedDescendantsBuildOneCurrentScaleAtlas()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
		for (qint64 row = 0; row < 2; ++row)
		{
			for (qint64 column = 0; column < 2; ++column)
			{
				online.insertTile(
					{ 2, column, row },
					{ paddedImage({ 4, 4 }, QColor(20, 40, 60, 128)),
					  false, false });
			}
		}

		auto const window = OnlineRasterTemplate::TileWindow { 1, 0, 0, 0, 0 };
		online.wanted_window_ = window;
		auto const map_rect = mapRectForWindow(online, window);
		QVector<RasterTemplateTile> tiles;
		online.collectRasterTiles(map_rect, 1.0e-5, true, tiles);
		QVERIFY(std::ranges::any_of(
			tiles, [](auto const& tile) { return tile.missing; }));
		QTRY_VERIFY_WITH_TIMEOUT(
			online.atlas_.window == window && !online.atlas_.image.isNull(), 5000);

		tiles.clear();
		online.collectRasterTiles(map_rect, 1.0e-5, true, tiles);
		QCOMPARE(tiles.size(), 1);
		QVERIFY(!tiles.front().missing);
		QVERIFY(tiles.front().provisional);
		QVERIFY(tiles.front().has_image_to_map);
	}

	void decodeValidatesDimensionsAndBuildsGutters()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));

		online.queueDecode({ 2, 1, 1 }, encodedPng({ 4, 4 }, QColor(10, 20, 30, 128)),
						   online.generation_);
		QTRY_VERIFY_WITH_TIMEOUT(online.tile_cache_.contains({ 2, 1, 1 })
									 || online.failed_tiles_.contains({ 2, 1, 1 }),
								 5000);
		QVERIFY2(online.tile_cache_.contains({ 2, 1, 1 }),
				 online.failed_tiles_.contains({ 2, 1, 1 })
					 ? "The valid image was rejected by decodeTile"
					 : "The decode job did not complete");
		auto const* decoded = online.tile_cache_.object({ 2, 1, 1 });
		QVERIFY(decoded);
		QCOMPARE(decoded->image.size(), QSize(6, 6));
		QVERIFY(!decoded->opaque);
		QCOMPARE(decoded->image.pixelColor(0, 3), QColor(10, 20, 30, 128));
		QCOMPARE(decoded->image.pixelColor(5, 3), QColor(10, 20, 30, 128));

		online.queueDecode({ 2, 2, 1 }, encodedPng({ 3, 4 }, Qt::green), online.generation_);
		QTRY_VERIFY_WITH_TIMEOUT(online.failed_tiles_.contains({ 2, 2, 1 }), 5000);
		QVERIFY(!online.tile_cache_.contains({ 2, 2, 1 }));
	}

	void emptyTilesAreCompactAndDoNotForceTransparency()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
		online.insertEmptyTile({ 1, 0, 0 });
		online.insertTile({ 1, 1, 0 }, { paddedImage({ 4, 4 }, Qt::green), true, false });

		auto const* empty = online.tile_cache_.object({ 1, 0, 0 });
		QVERIFY(empty);
		QVERIFY(empty->empty);
		QVERIFY(empty->image.isNull());

		bool transparent = false;
		bool missing = false;
		bool pixels = false;
		auto visuals =
			online.visualTiles({ 1, 0, 1, 0, 0 }, false, &transparent, &missing, &pixels);
		QCOMPARE(visuals.size(), 2);
		QVERIFY(visuals.front().complete_empty);
		QVERIFY(!transparent);
		QVERIFY(!missing);
		QVERIFY(pixels);

		online.wanted_window_ = { 1, 0, 1, 0, 0 };
		auto const map_rect = mapRectForWindow(online, online.wanted_window_);
		QVector<RasterTemplateTile> tiles;
		online.collectRasterTiles(map_rect, 1.0e-5, true, tiles);
		QVERIFY(!tiles.isEmpty());
		QVERIFY(std::ranges::none_of(tiles, [](auto const& tile) { return tile.missing; }));
		QVERIFY(std::ranges::all_of(tiles, [](auto const& tile) { return tile.has_image_to_map; }));
	}

	void screenSceneIncludesAlreadyRequestedOverscan()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
		online.insertTile(
			{ 1, 0, 0 }, { paddedImage({ 4, 4 }, Qt::red), true, false }
		);
		online.insertTile(
			{ 1, 1, 0 }, { paddedImage({ 4, 4 }, Qt::blue), true, false }
		);
		online.wanted_window_ = { 1, 0, 1, 0, 0 };
		auto const visible = mapRectForWindow(online, { 1, 0, 0, 0, 0 });

		QVector<RasterTemplateTile> tiles;
		online.collectRasterTiles(visible, 1.0e-5, true, tiles);
		QCOMPARE(tiles.size(), 2);
		QVERIFY(std::ranges::all_of(
			tiles, [](auto const& tile) { return !tile.missing; }
		));
	}

	void screenWindowChangeDoesNotMutatePresentationState()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
		auto const visible = mapRectForWindow(online, { 1, 0, 0, 0, 0 });
		int invalidations = 0;
		connect(
			&map, &Map::templateAreaDirty, this,
			[&](Template* source, const QRectF&, int) {
				if (source == &online)
					++invalidations;
			});

		online.updateRenderContext({ visible, 1 });
		QCOMPARE(invalidations, 0);
		QVERIFY(!online.wanted_window_.isEmpty());
		online.updateRenderContext({ visible, 1 });
		QCOMPARE(invalidations, 0);
	}

		void workingSetBudgetPreventsCacheChurn()
		{
		Map map;
		OnlineRasterTemplate online(snapshotFixture(), &map);
		QVERIFY(online.workingSetFits({ 2, 0, 7, 0, 7 }));

		auto source = sourceFixture();
		for (auto& matrix : source.tile_matrix_set.matrices)
		{
			matrix.cell_size /= 128;
			matrix.tile_size = { 512, 512 };
		}
		OnlineRasterTemplate large_tiles(snapshotFixture(source), &map);
		QVERIFY(large_tiles.workingSetFits({ 2, 0, 7, 0, 7 }));
			QVERIFY(!large_tiles.workingSetFits({ 2, 0, 15, 0, 15 }));
		}

		void exactZoomUsesOneNumericalSafetyMargin()
		{
			Map map;
			georeferenceMap(map);
			OnlineRasterTemplate online(snapshotFixture(), &map);
			online.setTemplateState(Template::Unloaded);
			QVERIFY2(
				online.loadTemplateFile(),
				qPrintable(online.errorString()));
			auto const origin =
				online.mapToNominalSource({ 0, 0 });
			auto const x =
				online.mapToNominalSource({ 1, 0 });
			QVERIFY(origin);
			QVERIFY(x);
			auto const source_per_map_unit =
				QLineF(*origin, *x).length();
			auto const* target = online.matrix(1);
			QVERIFY(target);
			auto const desired_cell =
				target->cell_size / 0.97;
			auto const pixels_per_map_unit =
				source_per_map_unit / desired_cell;

			QCOMPARE(
				online.chooseZoom(
					QRectF(-1, -1, 2, 2),
					pixels_per_map_unit,
					true),
				1);
		}

		void staggeredRetryDeadlinesRemainScheduled()
	{
		Map map;
		OnlineRasterTemplate online(snapshotFixture(), &map);
		OnlineRasterTemplate::TileFailure first;
		first.attempts = 1;
		first.retry = QDeadlineTimer(50);
		online.failed_tiles_.insert({ 1, 0, 0 }, first);
		OnlineRasterTemplate::TileFailure second;
		second.attempts = 1;
		second.retry = QDeadlineTimer(500);
		online.failed_tiles_.insert({ 1, 1, 0 }, second);
		online.scheduleNextRetry();
		QVERIFY(online.retry_timer_.isActive());
		QTRY_VERIFY_WITH_TIMEOUT(
			online.retry_timer_.isActive()
			&& online.retry_timer_.remainingTime() > 100,
			300);
		QTRY_VERIFY_WITH_TIMEOUT(!online.retry_timer_.isActive(), 1000);
	}

		void partialPanCancelsButRetainsResidentDecodeAdmission()
		{
			Map map;
			OnlineRasterTemplate online(snapshotFixture(), &map);
		auto retained = std::make_shared<std::atomic_bool>(false);
		auto cancelled = std::make_shared<std::atomic_bool>(false);
		online.pending_decodes_.insert(
			{ 2, 1, 1 }, { retained, 0, 0 });
		online.pending_decodes_.insert(
			{ 2, 3, 3 }, { cancelled, 0, 0 });
		online.queued_tiles_ = { { 2, 1, 1 }, { 2, 3, 3 } };

			online.cancelUnwantedWork({ 2, 0, 2, 0, 2 });
			QVERIFY(!retained->load());
			QVERIFY(cancelled->load());
			QVERIFY(online.pending_decodes_.contains({ 2, 1, 1 }));
			QVERIFY(online.pending_decodes_.contains({ 2, 3, 3 }));
			QVERIFY(online.queued_tiles_.contains({ 2, 1, 1 }));
			QVERIFY(online.queued_tiles_.contains({ 2, 3, 3 }));
		}

		void offlineMissWaitsForConnectivityInsteadOfRetrying()
		{
			Map map;
			OnlineRasterTemplate online(snapshotFixture(), &map);
			auto const key = OnlineRasterTileKey{ 2, 1, 1 };
			constexpr imagery::TileNetworkManager::Token token = 42;
			online.pending_fetches_.insert(token, { key, online.generation_ });
			online.queued_tiles_.insert(key);

			imagery::TileNetworkResult miss;
			miss.outcome = imagery::TileNetworkResult::Outcome::OfflineMiss;
			miss.generation = online.generation_;
			online.onNetworkFinished(token, miss);

			QVERIFY(online.offline_tiles_.contains(key));
			QVERIFY(!online.retryAllowed(key));
			QVERIFY(!online.retry_timer_.isActive());
			QVERIFY(!online.queued_tiles_.contains(key));

			emit online.network_->offlineModeChanged(false);
			QVERIFY(!online.offline_tiles_.contains(key));
		}

		void exactOutputReportsAnOfflineCacheMiss()
		{
			Map map;
			georeferenceMap(map);
			auto source = sourceFixture();
			source.tile_matrix_set.matrices.resize(1);
			source.max_zoom = 0;
			OnlineRasterTemplate online(snapshotFixture(source), &map);
			online.setTemplateState(Template::Unloaded);
			QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
			auto const key = OnlineRasterTileKey{ 0, 0, 0 };
			online.offline_tiles_.insert(key);

			auto const result =
				online.prepareForOutput(mapRectForWindow(online, { 0, 0, 0, 0, 0 }), 100);
			QCOMPARE(result.state, OutputRenderPreparation::State::Failed);
			QVERIFY(result.error.contains(QStringLiteral("offline"), Qt::CaseInsensitive));
		}

		void exactOutputRejectsPartialProjectionCoverage()
		{
			Map map;
			georeferenceUtmMap(map);
			OnlineRasterTemplate online(snapshotFixture(geographicSourceFixture()), &map);
			online.setTemplateState(Template::Unloaded);
			QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));

			auto const result =
				online.prepareForOutput(QRectF(-1.0e12, -1.0e12, 2.0e12, 2.0e12), 1);
			QCOMPARE(result.state, OutputRenderPreparation::State::Failed);
			QVERIFY(result.error.contains(QStringLiteral("reproject"), Qt::CaseInsensitive));
		}

		void decodeBacklogHasAnEncodedByteBudget()
		{
			Map map;
			OnlineRasterTemplate online(snapshotFixture(), &map);
			OnlineRasterTemplate second(snapshotFixture(), &map);
			QCOMPARE(
				online.decode_bytes_in_flight_.get(),
				second.decode_bytes_in_flight_.get());
			auto const key = OnlineRasterTileKey{ 2, 1, 1 };
			online.decode_bytes_in_flight_->store(
				OnlineRasterTemplate::max_decode_encoded_bytes,
				std::memory_order_relaxed);
			online.queued_tiles_.insert(key);
			online.queueDecode(key, encodedPng({ 4, 4 }, Qt::green), online.generation_);

			QVERIFY(!online.pending_decodes_.contains(key));
			QVERIFY(!online.queued_tiles_.contains(key));
			auto const failure = online.failed_tiles_.constFind(key);
			QVERIFY(failure != online.failed_tiles_.cend());
			QVERIFY(!failure->permanent);
			online.decode_bytes_in_flight_->store(0, std::memory_order_relaxed);
		}

		void retainedRasterMemoryIsApplicationWide()
		{
			Map map;
			OnlineRasterTemplate first(snapshotFixture(), &map);
			OnlineRasterTemplate second(snapshotFixture(), &map);

			auto reservation = first.reserveRetainedMemory(
				OnlineRasterTemplate::max_retained_raster_bytes);
			QVERIFY(reservation);
			QVERIFY(!second.reserveRetainedMemory(1));
			reservation.reset();
			QVERIFY(second.reserveRetainedMemory(1));
		}

			void retainedRasterAdmissionEvictsStaleGlobalCacheEntries()
			{
			Map map;
			OnlineRasterTemplate first(snapshotFixture(), &map);
			OnlineRasterTemplate second(snapshotFixture(), &map);

			auto stale_reservation = first.reserveRetainedMemory(
				OnlineRasterTemplate::max_retained_raster_bytes);
			QVERIFY(stale_reservation);
			auto stale_tile = OnlineRasterTemplate::CachedTile {
				paddedImage({ 1, 1 }, Qt::red),
				true,
				false,
				std::move(stale_reservation),
			};
			QVERIFY(first.tile_cache_.insert(
				{ 0, 0, 0 },
				new OnlineRasterTemplate::CachedTile(
					std::move(stale_tile)),
				1));

			auto replacement = second.reserveRetainedMemory(1);
			QVERIFY(replacement);
				QVERIFY(first.tile_cache_.isEmpty());
			}

			void retainedRasterAdmissionSkipsExternallyPinnedCacheEntries()
			{
				Map map;
				OnlineRasterTemplate first(snapshotFixture(), &map);
				OnlineRasterTemplate second(snapshotFixture(), &map);
				auto const half =
					OnlineRasterTemplate::
						max_retained_raster_bytes
					/ 2;

				auto pinned = OnlineRasterTemplate::CachedTile {
					paddedImage({ 1, 1 }, Qt::red),
					true,
					false,
					first.reserveRetainedMemory(half),
				};
				auto reclaimable =
					OnlineRasterTemplate::CachedTile {
						paddedImage({ 1, 1 }, Qt::blue),
						true,
						false,
						first.reserveRetainedMemory(
							OnlineRasterTemplate::
								max_retained_raster_bytes
							- half),
					};
				QVERIFY(pinned.memory);
				QVERIFY(reclaimable.memory);
				QVERIFY(first.tile_cache_.insert(
					{ 0, 0, 0 },
					new OnlineRasterTemplate::CachedTile(
						pinned),
					1));
				first.output_tiles_.insert(
					{ 0, 0, 0 },
					pinned);
				QVERIFY(first.tile_cache_.insert(
					{ 0, 0, 1 },
					new OnlineRasterTemplate::CachedTile(
						std::move(reclaimable)),
					1));

				auto replacement =
					second.reserveRetainedMemory(1);
				QVERIFY(replacement);
				QVERIFY(first.tile_cache_.isEmpty());
				QVERIFY(
					first.output_tiles_
						.value({ 0, 0, 0 })
						.memory);
			}

		void retainedRasterAdmissionEvictsStaleGlobalAtlas()
		{
			Map map;
			OnlineRasterTemplate first(snapshotFixture(), &map);
			OnlineRasterTemplate second(snapshotFixture(), &map);

			first.atlas_.image =
				paddedImage({ 1, 1 }, Qt::blue);
			first.atlas_.memory = first.reserveRetainedMemory(
				OnlineRasterTemplate::max_retained_raster_bytes);
			QVERIFY(first.atlas_.memory);

			auto replacement = second.reserveRetainedMemory(1);
			QVERIFY(replacement);
			QVERIFY(first.atlas_.image.isNull());
			QVERIFY(!first.atlas_.memory);
		}

		void equivalentEndpointsFailOverBeforePermanentFailure()
		{
			Map map;
			auto source = sourceFixture();
			source.tile_urls.push_back({
				QStringLiteral(
					"https://tiles-backup.example.test/{z}/{x}/{y}.png"),
			});
			OnlineRasterTemplate online(
				snapshotFixture(source), &map);
			auto const key = OnlineRasterTileKey { 1, 0, 0 };

				online.recordEndpointFailure(
					key, 0, 0, true,
					QStringLiteral("bad endpoint"));
				QCOMPARE(
					online.failed_tiles_.value(key).terminal_endpoints.size(),
					1);
				QVERIFY(!online.failed_tiles_.value(key).permanent);

				online.recordEndpointFailure(
					key, 1, 1, true,
					QStringLiteral("bad backup"));
				QCOMPARE(
					online.failed_tiles_.value(key).terminal_endpoints.size(),
					2);
				QVERIFY(online.failed_tiles_.value(key).permanent);

				online.clearFailure(key);
				online.recordEndpointFailure(
					key, 0, 0, false,
					QStringLiteral("temporary outage"));
				QVERIFY(
					online.failed_tiles_.value(key)
						.terminal_endpoints.isEmpty());
				online.recordEndpointFailure(
					key, 1, 1, true,
					QStringLiteral("bad backup"));
				QCOMPARE(
					online.failed_tiles_.value(key).terminal_endpoints,
					QSet<int> { 1 });
				QVERIFY(!online.failed_tiles_.value(key).permanent);

				online.clearFailure(key);
				auto const private_url = QUrl(
					QStringLiteral(
						"http://192.168.50.2/tiles/1/0/0.png"));
				online.recordEndpointFailure(
					key, 0, 0, true,
					QStringLiteral("permission required"),
					private_url);
				online.recordEndpointFailure(
					key, 1, 1, true,
					QStringLiteral("bad backup"));
				QVERIFY(online.failed_tiles_.value(key).permanent);
				emit online.network_->privateOriginApprovalChanged(
					imagery::TileNetworkManager::canonicalOrigin(
						private_url),
					true);
				QVERIFY(!online.failed_tiles_.value(key).permanent);
				QVERIFY(
					!online.failed_tiles_.value(key)
						.terminal_endpoints.contains(0));

				online.clearFailure(key);
				constexpr imagery::TileNetworkManager::Token
					token = 9001;
				online.pending_fetches_.insert(
					token,
					{ key, online.generation_, 0, 0 });
				online.queued_tiles_.insert(key);
				imagery::TileNetworkResult empty_success;
				empty_success.outcome =
					imagery::TileNetworkResult::Outcome::Success;
				empty_success.generation =
					online.generation_;
				online.onNetworkFinished(
					token, empty_success);
				QCOMPARE(
					online.failed_tiles_.value(key)
						.terminal_endpoints,
					QSet<int> { 0 });
				QVERIFY(
					!online.failed_tiles_.value(key)
						.permanent);
			}

		void outputPreparationPinsOneExactZoomUntilDrawingFinishes()
	{
		Map map;
		georeferenceMap(map);
		auto source = sourceFixture();
		source.tile_matrix_set.matrices.resize(1);
		source.max_zoom = 0;
		OnlineRasterTemplate online(snapshotFixture(source), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
		online.insertTile({ 0, 0, 0 }, { paddedImage({ 4, 4 }, Qt::cyan), true, false });
		auto const map_rect = mapRectForWindow(online, { 0, 0, 0, 0, 0 });

		auto const preparation = online.prepareForOutput(map_rect, 100);
		QCOMPARE(preparation.state, OutputRenderPreparation::State::Ready);
		QCOMPARE(preparation.ready_resources, qsizetype(1));
		QCOMPARE(preparation.total_resources, qsizetype(1));
			QVERIFY(online.output_preparation_active_);
				QCOMPARE(online.output_window_.zoom, 0);
				QVERIFY(online.output_tiles_.contains({ 0, 0, 0 }));
				QVERIFY(
					online.output_render_memory_.contains(
						{ 0, 0, 0 }));

			// Screen activity may evict the ordinary cache after preflight.
			// Exact output keeps an implicitly shared pinned copy.
			online.tile_cache_.clear();

			QVector<RasterTemplateTile> tiles;
		online.collectRasterTiles(map_rect, 10'000, false, tiles);
		QVERIFY(!tiles.isEmpty());
			QVERIFY(std::ranges::none_of(
				tiles, [](auto const& tile) { return tile.missing || tile.provisional; }));
			QVERIFY(tiles.front().reserve_render_memory);
			auto const reserved =
				tiles.front().reserve_render_memory(
					qint64(tiles.front().image.width())
					* tiles.front().image.height() * 4);
			QCOMPARE(
				reserved.get(),
				online.output_render_memory_
					.value({ 0, 0, 0 }).get());
			online.finishOutputPreparation(false);
			QVERIFY(!online.output_preparation_active_);
			QVERIFY(online.output_keys_.isEmpty());
			QVERIFY(online.output_render_memory_.isEmpty());
	}

		void transparentOutputPreparationWaitsForDerivedAtlas()
		{
		Map map;
		georeferenceMap(map);
		auto source = sourceFixture();
		source.tile_matrix_set.matrices.resize(1);
		source.max_zoom = 0;
		OnlineRasterTemplate online(snapshotFixture(source), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
		online.insertTile({ 0, 0, 0 },
						  { paddedImage({ 4, 4 }, QColor(20, 40, 200, 128)), false, false });
		auto const map_rect = mapRectForWindow(online, { 0, 0, 0, 0, 0 });

		auto const first = online.prepareForOutput(map_rect, 1.0e-7);
		QCOMPARE(first.state, OutputRenderPreparation::State::Pending);
		QCOMPARE(first.total_resources, qsizetype(2));
			QTRY_VERIFY_WITH_TIMEOUT(online.prepareForOutput(map_rect, 1.0e-7).state
										 == OutputRenderPreparation::State::Ready,
									 5000);
			QVERIFY(online.output_source_tiles_released_);
			QVERIFY(online.output_tiles_.isEmpty());
			QVERIFY(!online.output_atlases_.isEmpty());
			QVERIFY(online.output_atlases_.front().render_memory);

			QVector<RasterTemplateTile> tiles;
			online.collectRasterTiles(map_rect, 1.0e-7, false, tiles);
			QCOMPARE(tiles.size(), 1);
		QVERIFY(!tiles.front().missing);
			QVERIFY(!tiles.front().provisional);
			auto const snapshot_memory =
				tiles.front().reserve_render_memory(
					qint64(tiles.front().image.width())
					* tiles.front().image.height()
					* 4);
			QCOMPARE(
				snapshot_memory.get(),
				online.output_atlases_.front()
					.render_memory.get());
				online.finishOutputPreparation(false);
			}

		void largeExactAtlasIsSplitIntoBoundedChunks()
		{
			Map map;
			auto source = sourceFixture();
			for (auto& matrix : source.tile_matrix_set.matrices)
				matrix.tile_size = { 256, 256 };
			OnlineRasterTemplate online(
				snapshotFixture(source), &map);
			auto const window =
				OnlineRasterTemplate::TileWindow {
					2, 0, 19, 0, 27
				};
			auto const chunks = online.atlasChunks(window, 1.0);
			QVERIFY(chunks.size() > 1);
			qint64 covered = 0;
			auto const* matrix = online.matrix(window.zoom);
			QVERIFY(matrix);
			for (auto const& chunk : chunks)
			{
				auto const count = online.tileCount(chunk);
				QVERIFY(count);
				covered += *count;
				auto const width =
					chunk.width()
					* matrix->tile_size.width();
				auto const height =
					chunk.height()
					* matrix->tile_size.height();
				QVERIFY(
					width
					<= OnlineRasterTemplate::
						max_atlas_dimension);
				QVERIFY(
					height
					<= OnlineRasterTemplate::
						max_atlas_dimension);
				QVERIFY(
					width * height
					<= OnlineRasterTemplate::
						max_atlas_pixels);
			}
			QCOMPARE(covered, qint64(20 * 28));
#ifndef Q_OS_ANDROID
			QVERIFY(online.workingSetFits(window));
#endif
		}

		void preparedTransparentAtlasCoversMultipleOutputPages()
	{
		Map map;
		georeferenceMap(map);
		auto source = sourceFixture();
		source.tile_matrix_set.matrices.resize(2);
		source.max_zoom = 1;
		OnlineRasterTemplate online(snapshotFixture(source), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
		online.insertTile({ 1, 0, 0 },
						  { paddedImage({ 4, 4 }, QColor(220, 40, 40, 128)), false, false });
		online.insertTile({ 1, 1, 0 },
						  { paddedImage({ 4, 4 }, QColor(40, 40, 220, 128)), false, false });
		auto const output_rect = mapRectForWindow(online, { 1, 0, 1, 0, 0 });
		constexpr auto output_scale = 2.0e-7;

		QCOMPARE(online.prepareForOutput(output_rect, output_scale).state,
				 OutputRenderPreparation::State::Pending);
		QTRY_VERIFY_WITH_TIMEOUT(online.prepareForOutput(output_rect, output_scale).state
									 == OutputRenderPreparation::State::Ready,
								 5000);
			QCOMPARE(online.output_atlases_.size(), 1);
			QVERIFY(online.output_atlases_.front().output_owned);
			auto const atlas_key =
				online.output_atlases_.front().image.cacheKey();

		// A non-output tile may complete while print progress processes events.
		// It must not invalidate the exact atlas pinned by this output session.
		online.insertTile({ 1, 0, 1 },
						  { paddedImage({ 4, 4 }, QColor(40, 220, 40, 128)), false, false });
			QCOMPARE(online.output_atlases_.size(), 1);
			QVERIFY(online.output_atlases_.front().output_owned);
			QCOMPARE(
				online.output_atlases_.front().image.cacheKey(),
				atlas_key);

		for (auto const page : {
				 OnlineRasterTemplate::TileWindow{ 1, 0, 0, 0, 0 },
				 OnlineRasterTemplate::TileWindow{ 1, 1, 1, 0, 0 },
			 })
		{
			QVector<RasterTemplateTile> tiles;
			online.collectRasterTiles(mapRectForWindow(online, page), output_scale, false, tiles);
			QCOMPARE(tiles.size(), 1);
			QVERIFY(!tiles.front().missing);
			QVERIFY(!tiles.front().provisional);
			QCOMPARE(tiles.front().image.cacheKey(), atlas_key);
			}
			online.finishOutputPreparation(false);
			QVERIFY(online.output_atlases_.isEmpty());
	}

	void cancellingOutputCancelsPendingAtlas()
	{
		Map map;
		georeferenceMap(map);
		auto source = sourceFixture();
		source.tile_matrix_set.matrices.resize(1);
		source.max_zoom = 0;
		OnlineRasterTemplate online(snapshotFixture(source), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
		online.insertTile({ 0, 0, 0 },
						  { paddedImage({ 4, 4 }, QColor(20, 40, 200, 128)), false, false });
		auto const map_rect = mapRectForWindow(online, { 0, 0, 0, 0, 0 });

		QCOMPARE(online.prepareForOutput(map_rect, 1.0e-7).state,
				 OutputRenderPreparation::State::Pending);
		QVERIFY(online.atlas_pending_for_output_);
			auto const cancelled = online.atlas_cancelled_;
			QVERIFY(cancelled);
			auto const owner_generation =
				online.atlas_owner_.generation();
			online.finishOutputPreparation(true);
			QVERIFY(cancelled->load(std::memory_order_relaxed));
			QVERIFY(
				online.atlas_owner_.generation()
				> owner_generation);
		QVERIFY(!online.atlas_cancelled_);
		QVERIFY(!online.atlas_pending_for_output_);
		QVERIFY(!online.output_preparation_active_);
	}

	void transparentWindowUsesOneSeamFreeAtlas()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));

		online.insertTile({ 1, 0, 0 }, { paddedImage({ 4, 4 }, QColor(255, 0, 0, 128)), false });
		online.insertTile({ 1, 1, 0 }, { paddedImage({ 4, 4 }, QColor(0, 0, 255, 128)), false });
		online.wanted_window_ = { 1, 0, 1, 0, 0 };
		auto const map_rect = mapRectForWindow(online, online.wanted_window_);

		QVector<RasterTemplateTile> tiles;
		online.collectRasterTiles(map_rect, 2.0e-7, true, tiles);
		QTRY_VERIFY_WITH_TIMEOUT(!online.atlas_.image.isNull(), 5000);
		tiles.clear();
		online.collectRasterTiles(map_rect, 2.0e-7, true, tiles);
		QCOMPARE(tiles.size(), 1);
		QVERIFY(!tiles.front().missing);
		QVERIFY(tiles.front().has_image_to_map);
		auto const& atlas = tiles.front().image;
		QVERIFY(atlas.width() >= 8);
		QVERIFY(atlas.height() >= 4);
		auto const y = atlas.height() / 2;
		auto const left = atlas.pixelColor(atlas.width() / 4, y);
		auto const right = atlas.pixelColor(3 * atlas.width() / 4, y);
		QVERIFY(left.red() > left.blue());
		QVERIFY(right.blue() > right.red());
		for (int x = atlas.width() / 2 - 1; x <= atlas.width() / 2 + 1; ++x)
		{
			QVERIFY(atlas.pixelColor(x, y).alpha() >= 120);
		}

		auto const completed_key = atlas.cacheKey();
		tiles.clear();
		online.collectRasterTiles(map_rect, 3.0e-7, true, tiles);
		QCOMPARE(tiles.size(), 1);
		QVERIFY(!tiles.front().missing);
		QVERIFY(tiles.front().provisional);
		QCOMPARE(tiles.front().image.cacheKey(), completed_key);
		QVERIFY(online.atlas_cancelled_);
		auto const pending = online.atlas_cancelled_;

		// A newer view and a newly arrived tile must not cancel the atlas that
		// is already making forward progress, nor clear the completed fallback.
		tiles.clear();
		online.collectRasterTiles(map_rect, 4.0e-7, true, tiles);
		QCOMPARE(online.atlas_cancelled_, pending);
		QCOMPARE(tiles.size(), 1);
		QCOMPARE(tiles.front().image.cacheKey(), completed_key);
		online.insertTile(
			{ 1, 0, 0 },
			{ paddedImage({ 4, 4 }, QColor(255, 255, 0, 128)), false });
		QCOMPARE(online.atlas_cancelled_, pending);
		QCOMPARE(online.atlas_.image.cacheKey(), completed_key);
		QTRY_VERIFY_WITH_TIMEOUT(!online.atlas_cancelled_, 5000);
	}

	void incompleteTransparentAtlasCannotReplaceCompleteCoverage()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));

		online.insertTile(
			{ 1, 0, 0 },
			{ paddedImage({ 4, 4 }, QColor(255, 0, 0, 128)), false });
		online.insertTile(
			{ 1, 1, 0 },
			{ paddedImage({ 4, 4 }, QColor(0, 0, 255, 128)), false });
		online.wanted_window_ = { 1, 0, 1, 0, 0 };
		auto map_rect = mapRectForWindow(online, online.wanted_window_);
		QVector<RasterTemplateTile> tiles;
		online.collectRasterTiles(map_rect, 2.0e-7, true, tiles);
		QTRY_VERIFY_WITH_TIMEOUT(
			online.atlas_.coverage_complete, 5000);
		auto const complete_key = online.atlas_.image.cacheKey();

		// Expanding into a row with no pixels may build a useful candidate, but
		// it must not atomically replace the complete atlas with transparent holes.
		online.wanted_window_ = { 1, 0, 1, 0, 1 };
		map_rect = mapRectForWindow(online, online.wanted_window_);
		tiles.clear();
		online.collectRasterTiles(map_rect, 2.0e-7, true, tiles);
		QCOMPARE(tiles.size(), 1);
		QCOMPARE(tiles.front().image.cacheKey(), complete_key);
		QVERIFY(online.atlas_cancelled_);
		QTRY_VERIFY_WITH_TIMEOUT(!online.atlas_cancelled_, 5000);
		QVERIFY(!online.atlas_deferred_signature_.isEmpty());
		QVERIFY(online.atlas_.coverage_complete);
		QCOMPARE(online.atlas_.image.cacheKey(), complete_key);

		// The deferred signature is not rebuilt on every redraw.
		tiles.clear();
		online.collectRasterTiles(map_rect, 2.0e-7, true, tiles);
		QVERIFY(!online.atlas_cancelled_);
		QCOMPARE(tiles.size(), 1);
		QCOMPARE(tiles.front().image.cacheKey(), complete_key);

		online.insertTile(
			{ 1, 0, 1 },
			{ paddedImage({ 4, 4 }, QColor(0, 255, 0, 128)), false });
		online.insertTile(
			{ 1, 1, 1 },
			{ paddedImage({ 4, 4 }, QColor(255, 255, 0, 128)), false });
		tiles.clear();
		online.collectRasterTiles(map_rect, 2.0e-7, true, tiles);
		QVERIFY(online.atlas_cancelled_);
		QTRY_VERIFY_WITH_TIMEOUT(
			online.atlas_.window == online.wanted_window_
				&& online.atlas_.coverage_complete,
			5000);
		QVERIFY(online.atlas_deferred_signature_.isEmpty());
		QVERIFY(online.atlas_.image.cacheKey() != complete_key);
	}

			void transparentCrossCrsWindowUsesCpuPrewarp()
	{
		Map map;
		georeferenceUtmMap(map);
		OnlineRasterTemplate online(snapshotFixture(geographicSourceFixture()), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
		online.insertTile({ 6, 20, 15 },
						  { paddedImage({ 4, 4 }, QColor(40, 180, 80, 140)), false, false });
		online.wanted_window_ = { 6, 20, 20, 15, 15 };
		auto const map_rect = mapRectForWindow(online, online.wanted_window_);
		bool transparent = false;
		bool missing = false;
		bool pixels = false;
		auto const visuals =
			online.visualTiles(online.wanted_window_, true, &transparent, &missing, &pixels);
		QVERIFY(transparent);
		QVERIFY(!missing);
		QVERIFY(pixels);
		auto const signature = online.atlasSignature(online.wanted_window_, visuals, false);

		double scale = 0;
		std::optional<OnlineRasterTemplate::AtlasBuildRequest> request;
		for (auto candidate : {
				 1.0e-5,
				 2.0e-5,
				 5.0e-5,
				 1.0e-4,
				 2.0e-4,
				 5.0e-4,
			 })
		{
			auto trial = online.makeAtlasBuildRequest(online.wanted_window_, visuals, false,
													  candidate, signature);
			if (trial && trial->warp)
			{
				scale = candidate;
				request = std::move(trial);
				break;
			}
		}
		QVERIFY2(request, "The cross-CRS fixture did not select CPU prewarp");
		QVERIFY(request->warp);
		QVERIFY(request->warp->columns >= 8);
		QVERIFY(request->warp->exact_ownership);
		QVERIFY(!request->warp->map_crs.isEmpty());
		QVERIFY(!request->warp->source_crs.isEmpty());

		QVector<RasterTemplateTile> tiles;
		online.collectRasterTiles(map_rect, scale, true, tiles);
		QTRY_VERIFY_WITH_TIMEOUT(!online.atlas_.image.isNull(), 5000);
		tiles.clear();
		online.collectRasterTiles(map_rect, scale, true, tiles);
		QCOMPARE(tiles.size(), 1);
		QVERIFY(!tiles.front().missing);
		QVERIFY(tiles.front().has_image_to_map);
			QVERIFY(!tiles.front().image.isNull());
		}

		void cpuPrewarpSamplesTrueNeighborPixelsAtChunkEdges()
		{
			OnlineRasterTemplate::AtlasBuildRequest request;
			request.window = { 0, 0, 0, 0, 0 };
			request.core_size = { 2, 1 };
			request.has_right_neighbor = true;
			request.visuals = {
				{
					QRectF(1, 1, 2, 1),
					paddedImage(
						{ 2, 1 },
						QColor(255, 0, 0, 128)),
					QRectF(1, 1, 2, 1),
					{},
				},
				{
					QRectF(3, 1, 2, 1),
					paddedImage(
						{ 2, 1 },
						QColor(0, 0, 255, 128)),
					QRectF(1, 1, 2, 1),
					{},
				},
			};
			OnlineRasterTemplate::AtlasWarpGrid warp;
			warp.columns = 1;
			warp.rows = 1;
			warp.output_size = { 2, 1 };
				warp.source_points = {
					QPointF(2, 1.5),
					QPointF(3, 1.5),
					QPointF(2, 1.5),
					QPointF(3, 1.5),
				};
				warp.exact_ownership = true;
				warp.map_crs = QStringLiteral("EPSG:3857");
				warp.source_crs = QStringLiteral("EPSG:3857");
				warp.map_to_projected = QTransform {};
				warp.map_bounds = QRectF(1, 0, 1, 1);
				warp.core_west = 0;
				warp.core_north = 1;
				warp.cell_size = 1;
				request.warp = std::move(warp);
				auto rejected_by_exact_ownership = request;
				rejected_by_exact_ownership.warp->map_bounds =
					QRectF(3, 0, 1, 1);

			auto cancelled =
				std::make_shared<std::atomic_bool>(false);
			std::optional<
				OnlineRasterTemplate::AtlasBuildResult>
				result;
			auto completed = false;
			auto owner =
				RasterResourceManager::instance()
					.createOwner(1);
			QVERIFY(
				RasterResourceManager::instance()
					.submit(
						owner,
						RasterResourceManager::Lane::
							Decode,
						RasterResourceManager::Priority::
							Visible,
						this,
						[request = std::move(request),
						 cancelled,
						 &result,
						 &completed](
							const RasterResourceManager::
								CancellationToken&
									cancellation) mutable {
							auto built =
								OnlineRasterTemplate::
									buildAtlas(
										std::move(
											request),
										cancelled,
										cancellation);
							return [
								&result,
								&completed,
								built =
									std::move(
										built)]()
								       mutable {
									       result =
										       std::move(
											       built);
									       completed =
										       true;
								       };
						}));
			QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
			QVERIFY(result);
			auto const edge =
				result->image.pixelColor(
					result->image.width() - 2,
					1);
				QVERIFY(edge.alpha() >= 120);
				QVERIFY(edge.red() > 20);
				QVERIFY(edge.blue() > 20);

				result.reset();
				completed = false;
				cancelled =
					std::make_shared<std::atomic_bool>(false);
				QVERIFY(
					RasterResourceManager::instance()
						.submit(
							owner,
							RasterResourceManager::Lane::Decode,
							RasterResourceManager::Priority::Visible,
							this,
							[request = std::move(
								rejected_by_exact_ownership),
							 cancelled,
							 &result,
							 &completed](
								const RasterResourceManager::
									CancellationToken&
										cancellation) mutable {
								auto built =
									OnlineRasterTemplate::buildAtlas(
										std::move(request),
										cancelled,
										cancellation);
								return [
									&result,
									&completed,
									built = std::move(built)]()
									       mutable {
										       result =
											       std::move(built);
										       completed = true;
									       };
							}));
				QTRY_VERIFY_WITH_TIMEOUT(completed, 5000);
				QVERIFY(result);
				QCOMPARE(
					result->image.pixelColor(1, 1).alpha(),
					0);
			}

	void panKeepsSourceGenerationAndCancelsOnlyIrrelevantFetches()
	{
		Map map;
		OnlineRasterTemplate online(snapshotFixture(), &map);
		auto const generation = online.generation_;
		QVERIFY(online.keyNeededForWindow({ 1, 0, 0 }, { 2, 0, 1, 0, 1 }));
		QVERIFY(!online.keyNeededForWindow({ 2, 3, 3 }, { 2, 0, 1, 0, 1 }));
		online.queueWindow({}, true);
		QCOMPARE(online.generation_, generation);
		QVERIFY(online.decode_owner_.isValid());
	}

	void georeferencingChangeInvalidatesOnlyDerivedAtlas()
	{
		Map map;
		georeferenceMap(map);
		OnlineRasterTemplate online(snapshotFixture(), &map);
		online.setTemplateState(Template::Unloaded);
		QVERIFY2(online.loadTemplateFile(), qPrintable(online.errorString()));
		online.insertTile({ 1, 0, 0 }, { paddedImage({ 4, 4 }, QColor(255, 0, 0, 128)), false });
		online.wanted_window_ = { 1, 0, 0, 0, 0 };
		auto const map_rect = mapRectForWindow(online, online.wanted_window_);
		QVector<RasterTemplateTile> tiles;
		online.collectRasterTiles(map_rect, 2.0e-7, true, tiles);
		QTRY_VERIFY_WITH_TIMEOUT(!online.atlas_.image.isNull(), 5000);
		QVERIFY(!online.atlas_.image.isNull());
		QVERIFY(online.tile_cache_.contains({ 1, 0, 0 }));

		online.onMapGeoreferencingChanged();
		QVERIFY(online.atlas_.image.isNull());
		QVERIFY(online.wanted_window_.isEmpty());
		QVERIFY(online.tile_cache_.contains({ 1, 0, 0 }));
	}
};

} // namespace OpenOrienteering

using OpenOrienteering::OnlineRasterTemplateTest;

QTEST_MAIN(OnlineRasterTemplateTest)
#include "online_raster_template_t.moc"
