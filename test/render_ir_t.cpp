/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render_ir_t.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include <QtTest>
#include <QColor>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QPainterPath>

#include "global.h"
#include "test_config.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "core/symbols/line_symbol.h"
#include "render/qpainter_renderer.h"
#include "render/qt_render_bridge.h"
#include "render/render_ir.h"
#include "render/render_snapshot.h"

using namespace OpenOrienteering;

namespace {

render::PathPtr rectangle(double left, double top, double right, double bottom)
{
	render::PathBuilder path(render::FillRule::Winding);
	path.moveTo({ left, top });
	path.lineTo({ right, top });
	path.lineTo({ right, bottom });
	path.lineTo({ left, bottom });
	path.close();
	return path.finish();
}

const RenderableVector* firstGeometry(const render::SnapshotObject& object)
{
	for (auto const& [priority, renderables] : object.colors)
	{
		Q_UNUSED(priority)
		if (renderables && !renderables->empty())
			return renderables.get();
	}
	return nullptr;
}

}  // namespace

void RenderIrTest::initTestCase()
{
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
	QDir::addSearchPath(
		QStringLiteral("testdata"),
		QDir(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR)).absoluteFilePath(QStringLiteral("data"))
	);
}

void RenderIrTest::immutableSnapshotSurvivesEdit()
{
	Map map;
	QVERIFY(map.loadFrom(QStringLiteral("testdata:symbols/line-symbol-border-variants.omap")));
	auto const first = map.publishRenderSnapshot();
	QVERIFY(first);
	QVERIFY(first->objectCount() >= 2);

	auto const cached = map.publishRenderSnapshot();
	QCOMPARE(cached.get(), first.get());
	QCOMPARE(cached->revision(), first->revision());

	std::map<render::ObjectId, const RenderableVector*> first_geometry;
	for (render::ObjectId id = 1; id <= first->maxObjectId(); ++id)
	{
		if (auto const* object = first->object(id))
			first_geometry.emplace(id, firstGeometry(*object));
	}
	QCOMPARE(first_geometry.size(), first->objectCount());

	render::RenderRequest request {
		render::fromQRectF(map.calculateExtent(true)),
		10,
		RenderConfig::HelperSymbols,
		1,
	};
	auto const old_ir = first->buildIR(request);
	QVERIFY(old_ir);
	auto const old_command_count = old_ir->commands.size();

	auto* edited = map.getPart(0)->getObject(0);
	QVERIFY(edited);
	edited->move(1000, 0);
	auto const second = map.publishRenderSnapshot();
	QVERIFY(second);
	QVERIFY(second->revision() > first->revision());
	QCOMPARE(second->objectCount(), first->objectCount());
	auto changed_geometry = std::size_t(0);
	auto shared_geometry = std::size_t(0);
	auto changed_objects = std::size_t(0);
	auto shared_objects = std::size_t(0);
	for (auto const& [id, geometry] : first_geometry)
	{
		auto const* first_object = first->object(id);
		auto const* second_object = second->object(id);
		QVERIFY(first_object);
		QVERIFY(second_object);
		QCOMPARE(second_object->id, first_object->id);
		if (second_object == first_object)
			++shared_objects;
		else
			++changed_objects;
		if (firstGeometry(*second_object) == geometry)
			++shared_geometry;
		else
			++changed_geometry;
	}
	QVERIFY(changed_geometry >= 1);
	QVERIFY(shared_geometry >= 1);
	QCOMPARE(changed_objects, std::size_t(1));
	QCOMPARE(shared_objects + changed_objects, first->objectCount());

	// The old revision remains complete and readable after the live object changed.
	auto const old_ir_again = first->buildIR(request);
	QCOMPARE(old_ir_again->revision, first->revision());
	QCOMPARE(old_ir_again->commands.size(), old_command_count);

	// Removal updates persistent object blocks and color-order buckets without
	// invalidating either earlier revision.
	auto const second_command_count = second->buildIR(request)->commands.size();
	map.getPart(0)->deleteObject(0);
	auto const third = map.publishRenderSnapshot();
	QCOMPARE(third->objectCount() + 1, second->objectCount());
	QVERIFY(third->revision() > second->revision());
	QVERIFY(third->buildIR(request)->commands.size() < second_command_count);
	QCOMPARE(first->buildIR(request)->commands.size(), old_command_count);
}

void RenderIrTest::curvedLineKeepsBothBorders()
{
	Map map;
	auto* fill = new MapColor;
	fill->setRgb(MapColorRgb(QColor(246, 218, 197)));
	map.addColor(fill, 0);
	auto* border = new MapColor;
	border->setRgb(MapColorRgb(Qt::black));
	map.addColor(border, 1);

	auto* symbol = new LineSymbol;
	symbol->setColor(fill);
	symbol->setLineWidth(0.55);
	symbol->setHasBorder(true);
	symbol->getBorder().color = border;
	symbol->getBorder().width = 70;
	symbol->getBorder().shift = 35;
	symbol->getRightBorder() = symbol->getBorder();
	map.addSymbol(symbol, 0);

	MapCoord start(-96.926, -52.501);
	start.setCurveStart(true);
	auto* object = new PathObject(symbol, {
		start,
		MapCoord(-96.327, -53.305),
		MapCoord(-95.862, -53.873),
		MapCoord(-95.644, -54.852),
	}, &map);
	map.addObject(object);

	auto const snapshot = map.publishRenderSnapshot();
	auto const ir = snapshot->buildIR({
		render::fromQRectF(map.calculateExtent(true).adjusted(-1, -1, 1, 1)),
		20,
		RenderConfig::NoOptions,
		1,
	});

	std::vector<QPainterPath> border_paths;
	for (auto const& command : ir->commands)
	{
		auto const* stroke = std::get_if<render::StrokePath>(&command);
		if (!stroke || std::abs(stroke->style.width - 0.07) > 1.0e-9 || !stroke->path)
		{
			continue;
		}
		QVERIFY(std::ranges::any_of(stroke->path->elements(), [](const auto& element) {
			return element.verb == render::PathVerb::CubicTo;
		}));
		border_paths.push_back(render::toQPainterPath(*stroke->path));
	}
	QCOMPARE(border_paths.size(), std::size_t(2));

	QPainterPath center;
	center.moveTo(-96.926, -52.501);
	center.cubicTo(-96.327, -53.305, -95.862, -53.873, -95.644, -54.852);
	auto signed_side = [&center](const QPainterPath& candidate, double percent) {
		const auto before = center.pointAtPercent(std::max(0.0, percent - 0.01));
		const auto after = center.pointAtPercent(std::min(1.0, percent + 0.01));
		const auto tangent = after - before;
		const auto right = QPointF{-tangent.y(), tangent.x()};
		const auto delta = candidate.pointAtPercent(percent) - center.pointAtPercent(percent);
		return QPointF::dotProduct(delta, right) / std::hypot(right.x(), right.y());
	};

	std::vector<double> middle_sides;
	for (auto const& border_path : border_paths)
	{
		const auto start_side = signed_side(border_path, 0);
		const auto middle_side = signed_side(border_path, 0.5);
		QVERIFY(std::abs(start_side) > 0.2);
		QVERIFY(std::abs(middle_side) > 0.2);
		QVERIFY(start_side * middle_side > 0);
		middle_sides.push_back(middle_side);
	}
	QVERIFY(middle_sides[0] * middle_sides[1] < 0);
}

void RenderIrTest::referenceRendererInterpretsIr()
{
	render::RenderIRBuilder builder(42, { 0, 0, 128, 128 });
	auto const full = rectangle(0, 0, 128, 128);
	auto const clip = rectangle(16, 16, 80, 80);
	auto const line = rectangle(4, 4, 124, 124);

	builder.fillPath(full, render::fromQColor(Qt::white));
	builder.pushTransform({ 1, 0, 0, 1, 4, 4 });
	builder.fillEllipse({ 4, 4, 12, 12 }, render::fromQColor(Qt::red));
	builder.popTransform();
	builder.pushClip(clip);
	builder.pushLayer(0.5);
	builder.fillPath(full, render::fromQColor(Qt::blue));
	builder.strokePath(line, render::fromQColor(Qt::black),
	                   { .width = 2, .cap = render::LineCap::Round,
	                     .join = render::LineJoin::Round, .miter_limit = 4 });
	builder.strokeEllipse({ 30, 30, 20, 20 }, render::fromQColor(Qt::yellow),
	                      { .width = 2, .cap = render::LineCap::Flat,
	                        .join = render::LineJoin::Miter, .miter_limit = 4 });
	builder.popLayer();
	builder.popClip();

	auto pixels = std::make_shared<const std::vector<std::uint8_t>>(
		std::vector<std::uint8_t> { 0, 255, 0, 255 }
	);
	auto image = std::make_shared<const render::ImageData>(render::ImageData { 1, 1, 4, pixels });
	builder.drawImage(image, { 96, 8, 16, 16 });
	builder.drawLinePattern(rectangle(88, 40, 120, 72), render::fromQColor(Qt::magenta),
	                        0, 4, 0, 1);

	auto const ir = builder.finish();
	QCOMPARE(ir->revision, render::Revision(42));
	QCOMPARE(ir->commands.size(), std::size_t(13));

	QImage output(128, 128, QImage::Format_ARGB32_Premultiplied);
	output.fill(Qt::transparent);
	QPainter painter(&output);
	render::QPainterRenderer().render(painter, *ir, true);
	painter.end();

	QCOMPARE(output.pixelColor(2, 2), QColor(Qt::white));
	QCOMPARE(output.pixelColor(10, 10), QColor(Qt::red));
	QVERIFY(output.pixelColor(24, 24).blue() > 100);
	QCOMPARE(output.pixelColor(104, 16), QColor(Qt::green));
	QVERIFY(output.pixelColor(100, 48) != QColor(Qt::white));
}

void RenderIrTest::antialiasPolicyPreservesCallerIntent()
{
	auto render_triangle = [](render::QualityHint quality, bool antialiasing_allowed) {
		render::PathBuilder path(render::FillRule::Winding);
		path.moveTo({ 1.25, 1.25 });
		path.lineTo({ 14.75, 1.25 });
		path.lineTo({ 1.25, 14.75 });
		path.close();
		render::RenderIRBuilder builder(1, { 0, 0, 16, 16 });
		builder.fillPath(path.finish(), render::fromQColor(Qt::black), quality);

		QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);
		QPainter painter(&image);
		Q_ASSERT(!painter.testRenderHint(QPainter::Antialiasing));
		render::QPainterRenderer().render(
			painter, *builder.finish(), antialiasing_allowed
		);
		painter.end();
		return image;
	};
	auto has_partial_alpha = [](const QImage& image) {
		for (auto y = 0; y < image.height(); ++y)
		{
			for (auto x = 0; x < image.width(); ++x)
			{
				auto const alpha = image.pixelColor(x, y).alpha();
				if (alpha > 0 && alpha < 255)
					return true;
			}
		}
		return false;
	};

	QVERIFY(!has_partial_alpha(render_triangle(render::QualityHint::Default, true)));
	QVERIFY(has_partial_alpha(render_triangle(render::QualityHint::ForceAntialiasing, true)));
	QVERIFY(!has_partial_alpha(render_triangle(render::QualityHint::ForceAntialiasing, false)));
}

#ifndef Q_OS_MACOS
namespace {
[[maybe_unused]] const auto qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");
}
#endif

QTEST_MAIN(RenderIrTest)
