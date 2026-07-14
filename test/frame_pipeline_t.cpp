/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "frame_pipeline_t.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <QtTest>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QTransform>

#include "global.h"
#include "test_config.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/map_view.h"
#include "core/objects/object.h"
#include "gui/map/map_widget.h"
#include "presentation/native_surface.h"
#include "render/frame_pipeline.h"
#include "render/qpainter_frame_renderer.h"
#include "render/qpainter_renderer.h"
#include "render/qt_render_bridge.h"

using namespace OpenOrienteering;

namespace {

struct Fixture
{
	Map map;
	QRectF extent;
	double scale = 10;
	QSize pixel_size;
	QTransform transform;
	render::RenderRequest render_request;
	render::FrameView view;
};

std::unique_ptr<Fixture> makeFixture(const QString& file_name)
{
	auto fixture = std::make_unique<Fixture>();
	if (!fixture->map.loadFrom(file_name))
		return {};
	fixture->extent = fixture->map.calculateExtent(true).toAlignedRect().adjusted(-1, -1, 1, 1);
	fixture->pixel_size = {
		std::max(1, qCeil(fixture->extent.width() * fixture->scale)),
		std::max(1, qCeil(fixture->extent.height() * fixture->scale)),
	};
	fixture->transform.scale(fixture->scale, fixture->scale);
	fixture->transform.translate(-fixture->extent.left(), -fixture->extent.top());
	fixture->render_request = {
		render::fromQRectF(fixture->extent),
		fixture->scale,
		RenderConfig::Screen | RenderConfig::HelperSymbols,
		1,
	};
	fixture->view = {
		std::uint32_t(fixture->pixel_size.width()),
		std::uint32_t(fixture->pixel_size.height()),
		1,
		render::fromQTransform(fixture->transform),
	};
	return fixture;
}

QImage renderDirect(const Fixture& fixture,
	                const render::MapRenderSnapshot& snapshot)
{
	QImage image(fixture.pixel_size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::white);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::TextAntialiasing, true);
	painter.setWorldTransform(fixture.transform);
	render::QPainterRenderer().draw(painter, snapshot, fixture.render_request);
	painter.end();
	return image;
}

QImage renderFrame(const Fixture& fixture, const render::FramePacket& frame)
{
	QImage image(fixture.pixel_size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::white);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::TextAntialiasing, true);
	auto const completion = render::QPainterFrameRenderer().render(painter, frame);
	Q_ASSERT(completion.frame_id == frame.id);
	Q_ASSERT(completion.status == render::FrameStatus::Presented);
	painter.end();
	return image;
}

QImage renderLegacyOverprint(const Fixture& fixture,
	                         const render::MapRenderSnapshot& snapshot)
{
	QImage image(fixture.pixel_size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::white);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::TextAntialiasing, true);
	painter.setWorldTransform(fixture.transform);
	auto const hints = painter.renderHints();
	auto const transform = painter.worldTransform();
	painter.save();
	painter.resetTransform();

	QImage separation(image.size(), QImage::Format_ARGB32_Premultiplied);
	for (auto color = snapshot.colors().rbegin(); color != snapshot.colors().rend(); ++color)
	{
		if (color->second.spot_method != render::SpotMethod::Spot)
			continue;
		separation.fill(Qt::transparent);
		QPainter separation_painter(&separation);
		separation_painter.setRenderHints(hints);
		separation_painter.setWorldTransform(transform);
		render::QPainterRenderer().drawColorSeparation(
			separation_painter, snapshot, fixture.render_request, color->first, true
		);
		separation_painter.end();
		painter.setCompositionMode(QPainter::CompositionMode_Multiply);
		painter.drawImage(0, 0, separation);
	}

	painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
	separation.fill(Qt::transparent);
	QPainter normal_painter(&separation);
	normal_painter.setRenderHints(hints);
	normal_painter.setWorldTransform(transform);
	auto spot_request = fixture.render_request;
	spot_request.options |= RenderConfig::RequireSpotColor;
	render::QPainterRenderer().draw(normal_painter, snapshot, spot_request);
	normal_painter.end();
	auto* pixel = reinterpret_cast<QRgb*>(separation.bits());
	auto const* end = pixel + separation.sizeInBytes() / sizeof(QRgb);
	for (; pixel < end; ++pixel)
		*pixel = (*pixel >> 2) & 0x3f3f3f3f;
	painter.drawImage(0, 0, separation);
	painter.restore();

	painter.setWorldTransform(transform);
	render::QPainterRenderer().drawColorSeparation(
		painter, snapshot, fixture.render_request, MapColor::Reserved, true
	);
	painter.end();
	return image;
}

QString example(const QString& name)
{
	auto const test_dir = QDir(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR));
	return QDir(test_dir.absoluteFilePath(QStringLiteral("../examples"))).absoluteFilePath(name);
}

}  // namespace

void FramePipelineTest::initTestCase()
{
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
}

void FramePipelineTest::packetIsCompleteAndMonotonic()
{
	auto fixture = makeFixture(example(QStringLiteral("overprinting.omap")));
	QVERIFY(fixture);
	auto snapshot = fixture->map.publishRenderSnapshot();
	QVERIFY(snapshot);

	render::FramePlanner planner;
	auto const normal = planner.plan(*snapshot, { fixture->view, fixture->render_request, false });
	auto const overprint = planner.plan(*snapshot, { fixture->view, fixture->render_request, true });
	QCOMPARE(normal->id, render::FrameId(1));
	QCOMPARE(overprint->id, render::FrameId(2));
	QCOMPARE(normal->revision, snapshot->revision());
	QCOMPARE(normal->vector_passes.size(), std::size_t(1));
	QVERIFY(overprint->vector_passes.size() > 1);
	QVERIFY(std::ranges::any_of(overprint->vector_passes, [](auto const& pass) {
		return pass.blend == render::BlendMode::Multiply;
	}));
	for (auto const& pass : overprint->vector_passes)
	{
		QVERIFY(pass.scene);
		QCOMPARE(pass.scene->revision, overprint->revision);
	}

	// The packet, not the live document, owns everything a backend reads.
	snapshot.reset();
	auto* object = fixture->map.getPart(0)->getObject(0);
	QVERIFY(object);
	object->move(1000, 0);
	fixture->map.publishRenderSnapshot();
	QVERIFY(!renderFrame(*fixture, *overprint).isNull());
}

void FramePipelineTest::qpainterConsumesTheFrameContract()
{
	auto fixture = makeFixture(example(QStringLiteral("complete map.omap")));
	QVERIFY(fixture);
	auto const snapshot = fixture->map.publishRenderSnapshot();
	render::FramePlanner planner;
	auto const frame = planner.plan(*snapshot, { fixture->view, fixture->render_request, false });
	QCOMPARE(renderFrame(*fixture, *frame), renderDirect(*fixture, *snapshot));

	QPainter inactive;
	auto const completion = render::QPainterFrameRenderer().render(inactive, *frame);
	QCOMPARE(completion.frame_id, frame->id);
	QCOMPARE(completion.status, render::FrameStatus::TargetUnavailable);
}

void FramePipelineTest::overprintingUsesIsolatedPasses()
{
	auto fixture = makeFixture(example(QStringLiteral("overprinting.omap")));
	QVERIFY(fixture);
	auto const snapshot = fixture->map.publishRenderSnapshot();
	render::FramePlanner planner;
	auto const frame = planner.plan(*snapshot, { fixture->view, fixture->render_request, true });
	QVERIFY(std::ranges::all_of(frame->vector_passes, [](auto const& pass) {
		return pass.blend != render::BlendMode::Multiply || pass.isolated;
	}));

	auto const actual = renderFrame(*fixture, *frame);
	auto const expected = renderLegacyOverprint(*fixture, *snapshot);
	auto changed = 0;
	auto max_delta = 0;
	for (auto y = 0; y < actual.height(); ++y)
	{
		for (auto x = 0; x < actual.width(); ++x)
		{
			auto const actual_color = actual.pixelColor(x, y);
			auto const expected_color = expected.pixelColor(x, y);
			if (actual_color == expected_color)
				continue;
			++changed;
			max_delta = std::max({
				max_delta,
				std::abs(actual_color.red() - expected_color.red()),
				std::abs(actual_color.green() - expected_color.green()),
				std::abs(actual_color.blue() - expected_color.blue()),
				std::abs(actual_color.alpha() - expected_color.alpha()),
			});
		}
	}
	QVERIFY2(changed == 0 && max_delta == 0,
	         qPrintable(QStringLiteral("%1 overprint pixels changed; max delta %2")
	                    .arg(changed).arg(max_delta)));
}

void FramePipelineTest::mapWidgetUsesTheFrameContract()
{
	auto fixture = makeFixture(example(QStringLiteral("complete map.omap")));
	QVERIFY(fixture);
	MapView view(nullptr, &fixture->map);
	MapWidget widget(false, true);
	widget.resize(800, 600);
	widget.setMapView(&view);
	widget.adjustViewToRect(fixture->extent, MapWidget::ContinuousZoom);
	widget.show();
	QVERIFY(QTest::qWaitForWindowExposed(&widget));
	QCoreApplication::processEvents();
	auto const captured = widget.grab();
	auto actual = captured.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);

	QImage cache(widget.size(), QImage::Format_ARGB32_Premultiplied);
	cache.fill(Qt::transparent);
	QPainter painter(&cache);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.translate(widget.width() / 2.0, widget.height() / 2.0);
	painter.setWorldTransform(view.worldTransform(), true);
	auto const visible = view.calculateViewedRect(widget.viewportToView(widget.rect()));
	fixture->map.draw(&painter, {
		fixture->map,
		visible,
		view.calculateFinalZoomFactor(),
		RenderConfig::Screen | RenderConfig::HelperSymbols,
		1,
	});
	painter.end();
	QImage expected(actual.size(), QImage::Format_ARGB32_Premultiplied);
	expected.setDevicePixelRatio(captured.devicePixelRatio());
	expected.fill(Qt::white);
	QPainter composite(&expected);
	composite.drawImage(0, 0, cache);
	composite.end();

	auto changed = 0;
	auto max_delta = 0;
	QPoint first_changed(-1, -1);
	for (auto y = 0; y < actual.height(); ++y)
	{
		for (auto x = 0; x < actual.width(); ++x)
		{
			auto const actual_color = actual.pixelColor(x, y);
			auto const expected_color = expected.pixelColor(x, y);
			if (actual_color == expected_color)
				continue;
			if (first_changed.x() < 0)
				first_changed = { x, y };
			++changed;
			max_delta = std::max({
				max_delta,
				std::abs(actual_color.red() - expected_color.red()),
				std::abs(actual_color.green() - expected_color.green()),
				std::abs(actual_color.blue() - expected_color.blue()),
				std::abs(actual_color.alpha() - expected_color.alpha()),
			});
		}
	}
	// QWidget's platform backing store can quantize antialiased edge pixels
	// differently from an in-memory QImage. Geometry and solid interiors must
	// still agree; only a small, low-delta edge band is accepted here. The
	// packet-to-QImage comparison above remains pixel exact.
	auto const changed_limit = actual.width() * actual.height() / 10;
	QVERIFY2(changed <= changed_limit && max_delta <= 8,
	         qPrintable(QStringLiteral("%1 pixels changed; max delta %2; first at %3,%4")
	                    .arg(changed).arg(max_delta)
	                    .arg(first_changed.x()).arg(first_changed.y())));
}

void FramePipelineTest::nativeSurfacePublishesOrderedLifecycle()
{
	using namespace presentation;
	NativeSurfaceWindow window;
	QVERIFY(window.flags().testFlag(Qt::WindowDoesNotAcceptFocus));
	std::vector<NativeSurfaceState> states;
	window.setStateHandler([&states](auto const& state) { states.push_back(state); });

	QPlatformSurfaceEvent created(QPlatformSurfaceEvent::SurfaceCreated);
	QCoreApplication::sendEvent(&window, &created);
	QVERIFY(states.size() >= 2);
	QVERIFY(states.back().phase == SurfacePhase::Hidden
	        || states.back().phase == SurfacePhase::Exposed);
	QVERIFY(states.back().native.window != 0);
#if defined(Q_OS_MACOS)
	QCOMPARE(states.back().native.platform, NativePlatform::AppKit);
#else
	QCOMPARE(states.back().native.platform, NativePlatform::Unknown);
#endif
	auto const created_sequence = states.back().sequence;

	window.resize(320, 200);
	QResizeEvent resized(QSize(320, 200), QSize());
	QCoreApplication::sendEvent(&window, &resized);
	QVERIFY(states.back().sequence > created_sequence);
	QCOMPARE(states.back().logical_width, std::uint32_t(320));
	QCOMPARE(states.back().logical_height, std::uint32_t(200));
	QVERIFY(states.back().physical_width >= states.back().logical_width);
	QVERIFY(states.back().physical_height >= states.back().logical_height);

#if defined(Q_OS_MACOS)
	auto frame_requests = 0;
	window.setFrameRequestHandler([&frame_requests] { ++frame_requests; });
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	QTRY_COMPARE(states.back().phase, SurfacePhase::Exposed);
	window.requestFrame();
	QTRY_COMPARE(frame_requests, 1);
	window.hide();
	QTRY_COMPARE(states.back().phase, SurfacePhase::Hidden);
#endif

	QPlatformSurfaceEvent destroying(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
	QCoreApplication::sendEvent(&window, &destroying);
	QCOMPARE(states.back().phase, SurfacePhase::Unavailable);
	QCOMPARE(states.back().native.window, std::uintptr_t(0));
	for (std::size_t i = 1; i < states.size(); ++i)
		QVERIFY(states[i].sequence > states[i - 1].sequence);

	std::vector<NativeSurfaceState> destruction_states;
	{
		auto retiring_window = std::make_unique<NativeSurfaceWindow>();
		retiring_window->setStateHandler([&destruction_states](auto const& state) {
			destruction_states.push_back(state);
		});
		QCoreApplication::sendEvent(retiring_window.get(), &created);
		QVERIFY(destruction_states.back().phase != SurfacePhase::Unavailable);
	}
	QCOMPARE(destruction_states.back().phase, SurfacePhase::Unavailable);
	QCOMPARE(destruction_states.back().native.window, std::uintptr_t(0));
}

namespace {
#ifndef Q_OS_MACOS
[[maybe_unused]] const auto qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
}

QTEST_MAIN(FramePipelineTest)
