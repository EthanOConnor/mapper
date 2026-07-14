/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render_ir_t.h"

#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include <QtTest>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRawFont>

#include "global.h"
#include "test_config.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
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

std::shared_ptr<const render::GlyphRun> testGlyphRun()
{
#if defined(Q_OS_MACOS)
	auto const file_name = QStringLiteral("/System/Library/Fonts/SFNSMono.ttf");
#elif defined(Q_OS_WIN)
	auto const file_name = QStringLiteral("C:/Windows/Fonts/arial.ttf");
#elif defined(Q_OS_ANDROID)
	auto const file_name = QStringLiteral("/system/fonts/Roboto-Regular.ttf");
#else
	auto const file_name = QStringLiteral("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
#endif
	QFile file(file_name);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	auto const data = file.readAll();
	QRawFont font(data, 20, QFont::PreferNoHinting);
	auto const indexes = font.glyphIndexesForString(QStringLiteral("A"));
	if (!font.isValid() || indexes.size() != 1)
		return {};

	auto mutable_bytes = std::make_shared<std::vector<std::uint8_t>>(std::size_t(data.size()));
	std::memcpy(mutable_bytes->data(), data.constData(), std::size_t(data.size()));
	auto bytes = std::shared_ptr<const std::vector<std::uint8_t>>(std::move(mutable_bytes));
	auto face = std::make_shared<const render::FontFace>(render::FontFace { bytes, 1 });
	return std::make_shared<const render::GlyphRun>(render::GlyphRun {
		face, 20, {}, { { indexes.front(), { 40, 115 } } }, false
	});
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

void RenderIrTest::referenceRendererInterpretsIr()
{
	render::RenderIRBuilder builder(42, { 0, 0, 128, 128 });
	auto const full = rectangle(0, 0, 128, 128);
	auto const clip = rectangle(16, 16, 80, 80);
	auto const line = rectangle(4, 4, 124, 124);

	builder.fillPath(full, render::fromQColor(Qt::white), 1);
	builder.pushTransform({ 1, 0, 0, 1, 4, 4 });
	builder.fillEllipse({ 4, 4, 12, 12 }, render::fromQColor(Qt::red), 2);
	builder.popTransform();
	builder.pushClip(clip);
	builder.pushLayer(0.5);
	builder.fillPath(full, render::fromQColor(Qt::blue), 3);
	builder.strokePath(line, render::fromQColor(Qt::black),
	                   { 2, render::LineCap::Round, render::LineJoin::Round, 4 }, 4);
	builder.strokeEllipse({ 30, 30, 20, 20 }, render::fromQColor(Qt::yellow),
	                      { 2, render::LineCap::Flat, render::LineJoin::Miter, 4 }, 5);
	builder.popLayer();
	builder.popClip();

	auto pixels = std::make_shared<const std::vector<std::uint8_t>>(
		std::vector<std::uint8_t> { 0, 255, 0, 255 }
	);
	auto image = std::make_shared<const render::ImageData>(render::ImageData { 1, 1, 4, pixels });
	builder.drawImage(image, { 96, 8, 16, 16 }, 1, 6);
	builder.drawLinePattern(rectangle(88, 40, 120, 72), render::fromQColor(Qt::magenta),
	                        0, 4, 0, 1, 7);

	auto const glyph_run = testGlyphRun();
	QVERIFY2(glyph_run, "The platform's standard test font is required");
	builder.drawGlyphRun(glyph_run, render::fromQColor(Qt::black), {}, false, 8);

	auto const ir = builder.finish();
	QCOMPARE(ir->revision, render::Revision(42));
	QCOMPARE(render::RenderIR::format_version, std::uint32_t(1));
	QVERIFY(ir->commands.size() >= 14);

	QImage output(128, 128, QImage::Format_ARGB32_Premultiplied);
	output.fill(Qt::transparent);
	QPainter painter(&output);
	render::RenderRequest request {
		{ 0, 0, 128, 128 },
		1,
		RenderConfig::NoOptions,
		1,
	};
	render::QPainterRenderer().render(painter, *ir, request);
	painter.end();

	QCOMPARE(output.pixelColor(2, 2), QColor(Qt::white));
	QCOMPARE(output.pixelColor(10, 10), QColor(Qt::red));
	QVERIFY(output.pixelColor(24, 24).blue() > 100);
	QCOMPARE(output.pixelColor(104, 16), QColor(Qt::green));
	QVERIFY(output.pixelColor(100, 48) != QColor(Qt::white));
	auto glyph_pixels = 0;
	for (auto y = 90; y < 120; ++y)
	{
		for (auto x = 32; x < 64; ++x)
			glyph_pixels += output.pixelColor(x, y) != QColor(Qt::white);
	}
	QVERIFY(glyph_pixels > 10);
}

#ifndef Q_OS_MACOS
namespace {
[[maybe_unused]] const auto qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");
}
#endif

QTEST_MAIN(RenderIrTest)
