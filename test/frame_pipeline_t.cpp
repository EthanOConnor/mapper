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
#include <QPen>
#include <QPlatformSurfaceEvent>
#include <QPixmap>
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
#include "presentation/vello_canvas.h"
#include "render/frame_pipeline.h"
#include "render/overlay_scene.h"
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

void FramePipelineTest::viewportOverlayUsesTheSharedFrameContract()
{
	constexpr auto width = 180;
	constexpr auto height = 120;
	render::OverlaySceneBuilder builder;
	builder.begin(41, { 0, 0, width, height });
	QPen pen(QColor(220, 20, 30), 3, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin);
	builder.setPen(pen);
	builder.setBrush(QColor(20, 180, 90, 180));
	builder.setOpacity(0.75);
	builder.translate(18, 13);
	QTransform rotation;
	rotation.rotate(17);
	builder.setWorldTransform(rotation, true);
	builder.drawRect(QRectF(8, 7, 54, 31));
	auto scene = builder.finish();

	render::FrameRequest request;
	request.view = {
		width,
		height,
		1,
		render::fromQTransform(QTransform::fromTranslate(80, 0)),
	};
	request.above_map.push_back({
		scene,
		render::BlendMode::SourceOver,
		1,
		false,
		render::VectorPass::Space::Viewport,
	});
	render::FramePlanner planner;
	auto const first = planner.plan(request);
	auto const second = planner.plan(request);
	QCOMPARE(first->id, render::FrameId(1));
	QCOMPARE(second->id, render::FrameId(2));
	QCOMPARE(first->revision, render::Revision(41));

	QImage actual(width, height, QImage::Format_ARGB32_Premultiplied);
	actual.fill(Qt::transparent);
	QPainter actual_painter(&actual);
	actual_painter.setRenderHint(QPainter::Antialiasing, true);
	QCOMPARE(render::QPainterFrameRenderer().render(actual_painter, *first).status,
	         render::FrameStatus::Presented);
	actual_painter.end();

	QImage expected(width, height, QImage::Format_ARGB32_Premultiplied);
	expected.fill(Qt::transparent);
	QPainter expected_painter(&expected);
	expected_painter.setRenderHint(QPainter::Antialiasing, true);
	expected_painter.setPen(pen);
	expected_painter.setBrush(QColor(20, 180, 90, 180));
	expected_painter.setOpacity(0.75);
	expected_painter.translate(18, 13);
	expected_painter.setWorldTransform(rotation, true);
	expected_painter.drawRect(QRectF(8, 7, 54, 31));
	expected_painter.end();
	auto pixelBounds = [](const QImage& image) {
		QRect bounds;
		for (auto y = 0; y < image.height(); ++y)
		{
			for (auto x = 0; x < image.width(); ++x)
			{
				if (image.pixelColor(x, y).alpha() > 0)
					bounds = bounds.united(QRect(x, y, 1, 1));
			}
		}
		return bounds;
	};
	QCOMPARE(pixelBounds(actual), pixelBounds(expected));
	auto max_delta = 0;
	auto changed_pixels = 0;
	auto total_delta = std::uint64_t(0);
	for (auto y = 0; y < height; ++y)
	{
		for (auto x = 0; x < width; ++x)
		{
			auto const a = actual.pixel(x, y);
			auto const b = expected.pixel(x, y);
			auto const delta = std::array {
				std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)),
				std::abs(qBlue(a) - qBlue(b)), std::abs(qAlpha(a) - qAlpha(b)),
			};
			max_delta = std::max(max_delta, *std::ranges::max_element(delta));
			total_delta += std::uint64_t(delta[0] + delta[1] + delta[2] + delta[3]);
			changed_pixels += a != b;
		}
	}
	auto const mean_delta = double(total_delta) / double(width * height * 4);
	QVERIFY2(max_delta <= 3 && mean_delta < 0.01,
	         qPrintable(QStringLiteral("%1 changed pixels; max channel delta %2; mean %3")
	                    .arg(changed_pixels).arg(max_delta).arg(mean_delta)));
}

void FramePipelineTest::overlayPatternsAndImagesStayRetained()
{
	render::OverlaySceneBuilder builder;
	QPixmap cursor(24, 16);
	cursor.setDevicePixelRatio(2);
	cursor.fill(QColor(20, 40, 60, 180));

	auto record = [&] {
		builder.begin(42, { 0, 0, 80, 60 });
		builder.setPen(Qt::NoPen);
		builder.setBrush(QBrush(QColor(180, 20, 80), Qt::Dense5Pattern));
		builder.drawRect(QRectF(4, 5, 30, 20));
		builder.drawPixmap(QPointF(40, 9), cursor);
		return builder.finish();
	};

	auto const first = record();
	auto const second = record();
	auto pattern_count = 0;
	std::shared_ptr<const render::ImageData> first_image;
	for (auto const& command : first->commands)
	{
		if (std::holds_alternative<render::DrawLinePattern>(command))
			++pattern_count;
		if (auto const* image = std::get_if<render::DrawImage>(&command))
		{
			first_image = image->image;
			QCOMPARE(image->target.width, 12.0);
			QCOMPARE(image->target.height, 8.0);
		}
	}
	QCOMPARE(pattern_count, 2);
	QVERIFY(first_image);

	std::shared_ptr<const render::ImageData> second_image;
	for (auto const& command : second->commands)
		if (auto const* image = std::get_if<render::DrawImage>(&command))
			second_image = image->image;
	QCOMPARE(second_image, first_image);
}

void FramePipelineTest::mapWidgetUsesTheFrameContract()
{
	auto fixture = makeFixture(example(QStringLiteral("complete map.omap")));
	QVERIFY(fixture);
	MapView view(nullptr, &fixture->map);
	MapWidget widget(false, true);
	widget.setWindowFlag(Qt::WindowStaysOnTopHint);
	widget.resize(800, 600);
	widget.setMapView(&view);
	widget.adjustViewToRect(fixture->extent, MapWidget::ContinuousZoom);
	auto* canvas_widget = widget.findChild<QWidget*>(QStringLiteral("mapVelloCanvas"));
	QVERIFY(canvas_widget);
	auto* canvas = dynamic_cast<presentation::VelloCanvas*>(canvas_widget);
	QVERIFY(canvas);
	QTRY_VERIFY_WITH_TIMEOUT(canvas->currentFrame(), 5000);
	widget.show();
	QVERIFY(QTest::qWaitForWindowExposed(&widget));
	widget.raise();
	widget.activateWindow();
	widget.setCursor(Qt::CrossCursor);
	QCOMPARE(canvas->presentationCursor().shape(), Qt::CrossCursor);
	QPixmap custom_cursor(16, 16);
	custom_cursor.fill(QColor(40, 90, 180));
	widget.setCursor(QCursor(custom_cursor, 3, 5));
	QCOMPARE(canvas->presentationCursor().shape(), Qt::BitmapCursor);
	QCOMPARE(canvas->presentationCursor().hotSpot(), QPoint(3, 5));
	QCOMPARE(canvas->presentationCursor().pixmap().toImage(), custom_cursor.toImage());
	presentation::NativeSurfaceWindow* native_surface = nullptr;
	for (auto* window : QGuiApplication::allWindows())
	{
		auto* candidate = dynamic_cast<presentation::NativeSurfaceWindow*>(window);
		if (candidate
		    && candidate->surfaceState().native.window == canvas->surfaceState().native.window)
		{
			native_surface = candidate;
			break;
		}
	}
	QVERIFY(native_surface);
	auto const drag_start = QPointF(300, 240);
	auto const drag_end = QPointF(365, 275);
	auto const global_start = QPointF(native_surface->mapToGlobal(drag_start.toPoint()));
	auto const global_end = QPointF(native_surface->mapToGlobal(drag_end.toPoint()));
	auto const center_before_drag = view.center();
	QMouseEvent press(
		QEvent::MouseButtonPress, drag_start, drag_start, global_start,
		Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier
	);
	QCoreApplication::sendEvent(native_surface, &press);
	QCOMPARE(canvas->presentationCursor().shape(), Qt::ClosedHandCursor);
	QMouseEvent move(
		QEvent::MouseMove, drag_end, drag_end, global_end,
		Qt::NoButton, Qt::MiddleButton, Qt::NoModifier
	);
	QCoreApplication::sendEvent(native_surface, &move);
	QCOMPARE(view.panOffset(), (drag_end - drag_start).toPoint());
	QMouseEvent release(
		QEvent::MouseButtonRelease, drag_end, drag_end, global_end,
		Qt::MiddleButton, Qt::NoButton, Qt::NoModifier
	);
	QCoreApplication::sendEvent(native_surface, &release);
	QCOMPARE(view.panOffset(), QPoint());
	QVERIFY(view.center() != center_before_drag);
	QCOMPARE(canvas->presentationCursor().shape(), Qt::BitmapCursor);
	auto const frame_is_current = [canvas] {
		return canvas->currentFrame() && canvas->lastResult()
		       && canvas->lastResult()->completion.frame_id == canvas->currentFrame()->id
		       && canvas->lastResult()->surface_sequence == canvas->surfaceState().sequence
		       && canvas->lastResult()->completion.status == render::FrameStatus::Presented;
	};
	auto const state_description = [canvas] {
		auto const frame_id = canvas->currentFrame() ? canvas->currentFrame()->id : 0;
		auto const result_id = canvas->lastResult()
		                     ? canvas->lastResult()->completion.frame_id : 0;
		auto const result_status = canvas->lastResult()
		                         ? int(canvas->lastResult()->completion.status) : -1;
		auto const result_surface = canvas->lastResult()
		                          ? canvas->lastResult()->surface_sequence : 0;
		return QStringLiteral(
			"frame %1, result %2 status %3, result surface %4, current surface %5 phase %6, error: %7"
		).arg(frame_id)
		 .arg(result_id)
		 .arg(result_status)
		 .arg(result_surface)
		 .arg(canvas->surfaceState().sequence)
		 .arg(int(canvas->surfaceState().phase))
		 .arg(QString::fromStdString(canvas->lastError()));
	};
	QTRY_VERIFY2_WITH_TIMEOUT(
		frame_is_current(),
		qPrintable(state_description()),
		30000
	);
	auto const first_frame = canvas->currentFrame();
	QVERIFY(!first_frame->vector_passes.empty());
	QVERIFY2(canvas->lastError().empty(), canvas->lastError().c_str());

	auto* object = fixture->map.getPart(0)->getObject(0);
	QVERIFY(object);
	object->move(1000, 0);
	widget.updateEverything();
	QTRY_VERIFY(canvas->currentFrame()->id > first_frame->id);
	QVERIFY(canvas->currentFrame()->revision >= first_frame->revision);
	QTRY_VERIFY2_WITH_TIMEOUT(
		frame_is_current(),
		qPrintable(state_description()),
		30000
	);
}

void FramePipelineTest::nativeSurfacePublishesOrderedLifecycle()
{
	using namespace presentation;
	NativeSurfaceWindow window;
	QVERIFY(window.flags().testFlag(Qt::WindowDoesNotAcceptFocus));
	QVERIFY(!window.flags().testFlag(Qt::WindowTransparentForInput));
	auto input_events = 0;
	window.setInputHandler([&input_events](QEvent* event) {
		if (event->type() != QEvent::MouseMove)
			return false;
		++input_events;
		return true;
	});
	QMouseEvent mouse_move(
		QEvent::MouseMove, QPointF(20, 30), QPointF(20, 30),
		Qt::NoButton, Qt::NoButton, Qt::NoModifier
	);
	QCoreApplication::sendEvent(&window, &mouse_move);
	QCOMPARE(input_events, 1);
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
#elif defined(Q_OS_WIN)
	QCOMPARE(states.back().native.platform, NativePlatform::Win32);
#elif defined(Q_OS_LINUX)
	auto const platform_name = QGuiApplication::platformName().toLower();
	if (platform_name == QLatin1String("xcb"))
		QCOMPARE(states.back().native.platform, NativePlatform::Xcb);
	else if (platform_name.contains(QLatin1String("wayland")))
		QCOMPARE(states.back().native.platform, NativePlatform::Wayland);
	else
		QFAIL(qPrintable(QStringLiteral("Unsupported native test platform: %1")
		                 .arg(platform_name)));
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
	auto const resized_state_count = states.size();
	QResizeEvent duplicate_resize(QSize(320, 200), QSize(320, 200));
	QCoreApplication::sendEvent(&window, &duplicate_resize);
	QCOMPARE(states.size(), resized_state_count);
	window.refreshState();
	QCOMPARE(states.size(), resized_state_count + 1);
	QVERIFY(states.back().sequence > created_sequence);

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

QTEST_MAIN(FramePipelineTest)
