/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "sketch_layer_t.h"

#include <cmath>
#include <variant>

#include <QBuffer>
#include <QtTest>

#include "collaboration/map_hub_edit_transaction.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "core/sketch_layer.h"
#include "core/symbols/sketch_symbol.h"
#include "render/render_snapshot.h"
#include "render/qt_render_bridge.h"

using namespace OpenOrienteering;

namespace {

PathObject* addStroke(
        Map& map, MapPart* layer, SketchSymbol* symbol,
        MapCoordVector coords)
{
	auto* stroke = new PathObject(symbol, std::move(coords), &map);
	map.addObject(stroke, map.findPartIndex(layer));
	map.setObjectsDirty();
	return stroke;
}

}  // namespace

void SketchLayerTest::createsOneLayerAndLazyStyles()
{
	Map map;
	QCOMPARE(map.getNumParts(), 1);
	auto* layer = SketchLayer::ensure(map);
	QVERIFY(layer);
	QCOMPARE(map.getNumParts(), 2);
	QCOMPARE(SketchLayer::ensure(map), layer);
	QCOMPARE(SketchLayer::find(map), layer);
	QCOMPARE(SketchLayer::partIndex(map), 1);
	QCOMPARE(layer->getName(), QStringLiteral("Sketch"));

	const QColor color(219, 0, 217, 210);
	auto* medium = SketchLayer::ensureSymbol(
	    map, color, SketchLayer::Width::Medium);
	QVERIFY(medium);
	QVERIFY(medium->isHelperSymbol());
	QVERIFY(medium->isProtected());
	QCOMPARE(medium->strokeColor().rgba64(), color.rgba64());
	QCOMPARE(
	    SketchLayer::ensureSymbol(map, color, SketchLayer::Width::Medium),
	    medium);
	auto* broad = SketchLayer::ensureSymbol(
	    map, color, SketchLayer::Width::Broad);
	QVERIFY(broad != medium);
	QCOMPARE(map.getNumSymbols(), 2);
}

void SketchLayerTest::roundTripsAsNativeVectorData()
{
	Map original;
	auto* layer = SketchLayer::ensure(original);
	const QColor color(12, 126, 240, 192);
	auto* symbol = SketchLayer::ensureSymbol(
	    original, color, SketchLayer::Width::Fine);
	auto* stroke = addStroke(
	    original, layer, symbol,
	    {MapCoord(-2500, 1000), MapCoord(0, -750), MapCoord(3200, 1500)});
	const auto stroke_id = stroke->persistentId();
	const auto symbol_id = symbol->persistentId();
	const auto layer_id = layer->persistentId();

	QBuffer output;
	QVERIFY(original.exportToIODevice(output));
	const auto bytes = output.data();
	QVERIFY(bytes.contains("kind=\"mapper-sketch-v1\""));
	QVERIFY(bytes.contains("<sketch_symbol"));
	QVERIFY(bytes.contains(color.name(QColor::HexArgb).toUtf8()));
	QVERIFY(!bytes.contains("data:image/png"));

	QBuffer input;
	input.setData(bytes);
	QVERIFY(input.open(QIODevice::ReadOnly));
	Map restored;
	QVERIFY(restored.importFromIODevice(input));
	auto* restored_layer = SketchLayer::find(restored);
	QVERIFY(restored_layer);
	QCOMPARE(restored_layer->persistentId(), layer_id);
	QCOMPARE(restored_layer->getNumObjects(), 1);
	QCOMPARE(restored_layer->getObject(0)->persistentId(), stroke_id);
	const auto* restored_symbol = dynamic_cast<const SketchSymbol*>(
	    restored_layer->getObject(0)->getSymbol());
	QVERIFY(restored_symbol);
	QCOMPARE(restored_symbol->persistentId(), symbol_id);
	QCOMPARE(restored_symbol->strokeColor().rgba64(), color.rgba64());
	QCOMPARE(
	    restored_symbol->getLineWidth(),
	    qRound(1000 * SketchLayer::widthMillimeters(
	                      SketchLayer::Width::Fine)));
}

void SketchLayerTest::encodesAsConnectedEditingEntities()
{
	Map map;
	auto* layer = SketchLayer::ensure(map);
	const QColor color(8, 92, 230, 176);
	auto* symbol = SketchLayer::ensureSymbol(
	    map, color, SketchLayer::Width::Medium);
	auto* stroke = addStroke(
	    map, layer, symbol,
	    {MapCoord(-1000, 0), MapCoord(1000, 0)});

	MapHubEditTransaction transaction;
	transaction.client_instance_id =
	    QStringLiteral("40000000-0000-4000-8000-000000000101");
	transaction.client_sequence = 1;
	transaction.transaction_id =
	    QStringLiteral("40000000-0000-4000-8000-000000000102");
	transaction.expected_stream_hash = QString(64, QLatin1Char('0'));
	transaction.expected_workspace_revision_id =
	    QStringLiteral("50000000-0000-4000-8000-000000000101");
	transaction.expected_project_revision_id =
	    QStringLiteral("50000000-0000-4000-8000-000000000102");
	transaction.operations = {
	    {MapHubEditOperation::Kind::PutPart,
	     layer->persistentId(), {}, {}, 0, layer->getName()},
	    {MapHubEditOperation::Kind::PutSymbol,
	     symbol->persistentId(), {}, {}, 0,
	     MapHubEditTransaction::symbolFragment(*symbol, map)},
	    {MapHubEditOperation::Kind::PutObject,
	     stroke->persistentId(), layer->persistentId(), {}, 0,
	     MapHubEditTransaction::objectFragment(*stroke)},
	};

	QString error;
	QVERIFY2(transaction.isValid(&error), qPrintable(error));
	const auto bytes = transaction.canonicalBytes(&error);
	QVERIFY2(!bytes.isEmpty(), qPrintable(error));
	QVERIFY(bytes.contains("\"part.put\""));
	QVERIFY(bytes.contains("\"symbol.put\""));
	QVERIFY(bytes.contains("\"object.put\""));
	QVERIFY(bytes.contains("mapper-sketch-v1"));
	QVERIFY(bytes.contains(color.name(QColor::HexArgb).toUtf8()));
	QCOMPARE(map.getNumColors(), 0);
}

void SketchLayerTest::simplifiesDenseInputWithoutRasterStorage()
{
	std::vector<MapCoordF> sampled;
	sampled.reserve(4000);
	for (int i = 0; i < 4000; ++i)
	{
		const auto x = qreal(i) * 4;
		sampled.emplace_back(x, 500 * std::sin(x / 450));
	}
	const auto simplified = SketchLayer::simplify(sampled, 8);
	QVERIFY(simplified.size() < sampled.size() / 10);
	QCOMPARE(simplified.front(), sampled.front());
	QCOMPARE(simplified.back(), sampled.back());

	Map map;
	auto* layer = SketchLayer::ensure(map);
	auto* symbol = SketchLayer::ensureSymbol(
	    map, QColor(Qt::red), SketchLayer::Width::Medium);
	MapCoordVector coords;
	coords.reserve(simplified.size());
	for (const auto& point : simplified)
		coords.emplace_back(point);
	addStroke(map, layer, symbol, std::move(coords));

	QBuffer output;
	QVERIFY(map.exportToIODevice(output));
	QVERIFY(output.data().size() < 32 * 1024);
	QVERIFY(!output.data().contains(".png"));
	QVERIFY(!output.data().contains("Draft @"));
}

void SketchLayerTest::erasesContinuousCrossingsBetweenSparseEvents()
{
	Map map;
	auto* layer = SketchLayer::ensure(map);
	auto* symbol = SketchLayer::ensureSymbol(
	    map, QColor(Qt::black), SketchLayer::Width::Medium);
	auto* stroke = addStroke(
	    map, layer, symbol,
	    {MapCoord(-5000, 0), MapCoord(5000, 0)});

	QVERIFY(SketchLayer::intersectsStroke(
	    {MapCoordF(0, -10), MapCoordF(0, 10)}, *stroke, 0.001));
	QVERIFY(SketchLayer::intersectsStroke(
	    {MapCoordF(-10, 1), MapCoordF(10, 1)}, *stroke, 1));
	QVERIFY(!SketchLayer::intersectsStroke(
	    {MapCoordF(-10, 1.001), MapCoordF(10, 1.001)},
	    *stroke, 1));
}

void SketchLayerTest::rendersOnlyWithHelperLayersEnabled()
{
	Map map;
	auto* layer = SketchLayer::ensure(map);
	const QColor color(240, 38, 50, 180);
	auto* symbol = SketchLayer::ensureSymbol(
	    map, color, SketchLayer::Width::Broad);
	addStroke(
	    map, layer, symbol,
	    {MapCoord(-1000, 0), MapCoord(0, 500), MapCoord(1000, 0)});

	const auto snapshot = map.publishRenderSnapshot();
	const auto bounds = render::fromQRectF(
	    map.calculateExtent(true).adjusted(-2, -2, 2, 2));
	const auto hidden = snapshot->buildIR(
	    {bounds, 20, RenderConfig::NoOptions, 1});
	QVERIFY(hidden->commands.empty());
	const auto visible = snapshot->buildIR(
	    {bounds, 20, RenderConfig::Tool, 1});
	QCOMPARE(visible->commands.size(), std::size_t(1));
	const auto* stroke =
	    std::get_if<render::StrokePath>(&visible->commands.front());
	QVERIFY(stroke);
	QCOMPARE(render::toQColor(stroke->color).rgba64(), color.rgba64());
	QVERIFY(stroke->style.width >=
	        SketchLayer::widthMillimeters(SketchLayer::Width::Broad));
}

QTEST_MAIN(SketchLayerTest)
