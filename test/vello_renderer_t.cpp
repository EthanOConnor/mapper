/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "vello_renderer_t.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QRawFont>
#include <QTransform>
#include <QVBoxLayout>

#include "global.h"
#include "test_config.h"
#include "core/map.h"
#include "presentation/vello_canvas.h"
#include "render/frame_pipeline.h"
#include "render/qpainter_frame_renderer.h"
#include "render/qt_render_bridge.h"
#include "render/render_ir.h"
#include "render/vello_renderer.h"

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

render::FramePacketPtr completeOperationFrame(render::FrameId id)
{
	render::RenderIRBuilder builder(42, { 0, 0, 128, 128 });
	auto const full = rectangle(0, 0, 128, 128);
	auto const clip = rectangle(16, 16, 80, 80);
	builder.fillPath(full, render::fromQColor(Qt::white), 1);
	builder.pushTransform({ 1, 0, 0, 1, 4, 4 });
	builder.fillEllipse({ 4, 4, 12, 12 }, render::fromQColor(Qt::red), 2);
	builder.popTransform();
	builder.pushClip(clip);
	builder.pushLayer(0.5);
	builder.fillPath(full, render::fromQColor(Qt::blue), 3);
	builder.strokePath(full, render::fromQColor(Qt::black),
	                   { .width = 2, .cap = render::LineCap::Round,
	                     .join = render::LineJoin::Round, .miter_limit = 4 }, 4);
	builder.strokeEllipse({ 30, 30, 20, 20 }, render::fromQColor(Qt::yellow),
	                      { .width = 2, .cap = render::LineCap::Flat,
	                        .join = render::LineJoin::Miter, .miter_limit = 4 }, 5);
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
	if (!glyph_run)
		return {};
	builder.drawGlyphRun(glyph_run, render::fromQColor(Qt::black), {}, false, 8);

	auto frame = std::make_shared<render::FramePacket>();
	frame->id = id;
	frame->revision = 42;
	frame->view = { 128, 128, 1, {} };
	frame->render_request = {
		{ 0, 0, 128, 128 }, 1, RenderConfig::NoOptions, 1,
	};
	frame->vector_passes.push_back({ builder.finish() });
	return frame;
}

QString example(const QString& name)
{
	auto const test_dir = QDir(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR));
	return QDir(test_dir.absoluteFilePath(QStringLiteral("../examples"))).absoluteFilePath(name);
}

render::FramePacketPtr mapFrame(Map& map, QSize viewport,
	                            double device_pixel_ratio = 1,
	                            double rotation_degrees = 0,
	                            bool simulate_overprinting = false)
{
	auto const snapshot = map.publishRenderSnapshot();
	if (!snapshot)
		return {};
	auto const extent = map.calculateExtent(true).toAlignedRect().adjusted(-1, -1, 1, 1);
	auto const scale = std::min(
		(viewport.width() - 40.0) / extent.width(),
		(viewport.height() - 40.0) / extent.height()
	);
	QTransform transform;
	transform.translate(viewport.width() / 2.0, viewport.height() / 2.0);
	transform.rotate(rotation_degrees);
	transform.scale(scale, scale);
	transform.translate(-extent.center().x(), -extent.center().y());
	render::FramePlanner planner;
	return planner.plan(*snapshot, {
		{
			std::uint32_t(viewport.width()),
			std::uint32_t(viewport.height()),
			device_pixel_ratio,
			render::fromQTransform(transform),
		},
		{
			render::fromQRectF(extent),
			scale,
			RenderConfig::Screen | RenderConfig::HelperSymbols,
			1,
		},
		simulate_overprinting,
	});
}

QImage referenceImage(const render::FramePacket& frame)
{
	auto const width = qCeil(frame.view.width * frame.view.device_pixel_ratio);
	auto const height = qCeil(frame.view.height * frame.view.device_pixel_ratio);
	QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(frame.view.device_pixel_ratio);
	image.fill(Qt::white);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::TextAntialiasing, true);
	auto const completion = render::QPainterFrameRenderer().render(painter, frame);
	Q_ASSERT(completion.status == render::FrameStatus::Presented);
	painter.end();
	return image;
}

QImage imageFromVello(const render::VelloImage& rendered)
{
	QImage image(
		rendered.rgba8.data(), int(rendered.width), int(rendered.height),
		int(rendered.width * 4), QImage::Format_RGBA8888
	);
	return image.copy();
}

struct ImageDifference
{
	double mean_channel_delta = 0;
	int high_delta_pixels = 0;
};

ImageDifference compareImages(const QImage& actual, const QImage& expected)
{
	Q_ASSERT(actual.size() == expected.size());
	auto total_delta = std::uint64_t(0);
	auto high_delta_pixels = 0;
	for (auto y = 0; y < actual.height(); ++y)
	{
		for (auto x = 0; x < actual.width(); ++x)
		{
			auto const left = actual.pixelColor(x, y);
			auto const right = expected.pixelColor(x, y);
			auto const delta = std::abs(left.red() - right.red())
			                 + std::abs(left.green() - right.green())
			                 + std::abs(left.blue() - right.blue())
			                 + std::abs(left.alpha() - right.alpha());
			total_delta += std::uint64_t(delta);
			high_delta_pixels += delta > 160;
		}
	}
	return {
		double(total_delta) / double(actual.width() * actual.height() * 4),
		high_delta_pixels,
	};
}

std::optional<render::VelloFrameResult> waitForTerminalResult(
	presentation::VelloCanvas& canvas, render::FrameId frame_id,
	std::chrono::milliseconds timeout = std::chrono::seconds(20))
{
	QElapsedTimer elapsed;
	elapsed.start();
	render::FrameId latest_seen = 0;
	std::size_t result_count = 0;
	std::size_t dropped_count = 0;
	while (elapsed.elapsed() < timeout.count())
	{
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		while (auto result = canvas.takeResult())
		{
			latest_seen = std::max(latest_seen, result->completion.frame_id);
			++result_count;
			dropped_count += result->completion.status == render::FrameStatus::DroppedStale;
			if (result->completion.frame_id == frame_id
			    && result->surface_sequence == canvas.surfaceState().sequence
			    && (result->completion.status == render::FrameStatus::Presented
			        || result->completion.status == render::FrameStatus::TargetUnavailable))
				return result;
		}
		QTest::qWait(5);
	}
	qWarning() << "Timed out awaiting terminal Vello frame" << frame_id
	           << "after" << result_count << "results; latest id" << latest_seen
	           << "dropped" << dropped_count << "renderer error" << canvas.lastError();
	return {};
}

}  // namespace

void VelloRendererTest::initTestCase()
{
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
}

void VelloRendererTest::typedEncoderRetainsImmutableScenes()
{
	auto const first = completeOperationFrame(1);
	QVERIFY2(first, "The platform's standard test font is required");
	presentation::NativeSurfaceState surface;
	surface.sequence = 1;
	surface.phase = presentation::SurfacePhase::Exposed;
	surface.native.window = 1;
	surface.logical_width = 128;
	surface.logical_height = 128;
	surface.physical_width = 128;
	surface.physical_height = 128;

	render::VelloRenderer renderer;
	QVERIFY(renderer.setSurface(surface));
	QVERIFY(renderer.submit(first, surface));
	QCOMPARE(renderer.encodedSceneCount(), std::size_t(1));
	QCOMPARE(renderer.cachedSceneCount(), std::size_t(1));

	auto second = std::make_shared<render::FramePacket>(*first);
	second->id = 2;
	second->view.world_to_viewport.dx = 3;
	QVERIFY(renderer.submit(second, surface));
	QCOMPARE(renderer.encodedSceneCount(), std::size_t(1));
	QCOMPARE(renderer.cachedSceneCount(), std::size_t(1));
}

void VelloRendererTest::missingNativeTargetRequestsLifecycleRecovery()
{
	auto const frame = completeOperationFrame(3);
	QVERIFY2(frame, "The platform's standard test font is required");
	presentation::NativeSurfaceState surface;
	surface.sequence = 1;
	surface.phase = presentation::SurfacePhase::Exposed;
	surface.native.window = 1;
	surface.logical_width = 128;
	surface.logical_height = 128;
	surface.physical_width = 128;
	surface.physical_height = 128;

	render::VelloRenderer renderer;
	QVERIFY(renderer.setSurface(surface));
	QVERIFY(renderer.submit(frame, surface));
	std::optional<render::VelloFrameResult> result;
	QElapsedTimer elapsed;
	elapsed.start();
	while (!result && elapsed.elapsed() < 5000)
	{
		result = renderer.takeResult();
		QTest::qWait(5);
	}
	QVERIFY2(result, renderer.lastError().c_str());
	QCOMPARE(result->completion.frame_id, frame->id);
	QCOMPARE(result->completion.status, render::FrameStatus::SurfaceLost);
	QVERIFY(!renderer.lastError().empty());
}

void VelloRendererTest::offscreenGpuMatchesReference()
{
	auto const frame = completeOperationFrame(7);
	QVERIFY2(frame, "The platform's standard test font is required");
	render::VelloRenderer renderer;
	auto const rendered = renderer.renderOffscreen(frame);
	QVERIFY2(rendered, renderer.lastError().c_str());
	QCOMPARE(rendered->width, std::uint32_t(128));
	QCOMPARE(rendered->height, std::uint32_t(128));
	auto const actual = imageFromVello(*rendered);
	auto const expected = referenceImage(*frame);

	QCOMPARE(actual.pixelColor(2, 2), QColor(Qt::white));
	QVERIFY(actual.pixelColor(10, 10).red() > 180);
	QVERIFY(actual.pixelColor(104, 16).green() > 180);
	QVERIFY(actual.pixelColor(100, 48) != QColor(Qt::white));

	auto const difference = compareImages(actual, expected);
	qInfo() << "Vello/QPainter mean channel delta" << difference.mean_channel_delta
	        << "high-delta pixels" << difference.high_delta_pixels;
	QVERIFY(difference.mean_channel_delta < 3);
	QVERIFY(difference.high_delta_pixels < actual.width() * actual.height() / 50);
}

void VelloRendererTest::mapCorpusMatchesReference()
{
	struct Scenario
	{
		QString file;
		double device_pixel_ratio;
		double rotation_degrees;
		bool overprinting;
	};
	auto const scenarios = std::vector<Scenario> {
		{ example(QStringLiteral("complete map.omap")), 1, 0, false },
		{ example(QStringLiteral("complete map.omap")), 2, 17, false },
		{ example(QStringLiteral("overprinting.omap")), 1, 0, true },
		{ example(QStringLiteral("overprinting.omap")), 2, -11, true },
		{
			QDir(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR)).absoluteFilePath(
				QStringLiteral("data/symbols/line-symbol-border-variants.omap")
			),
			1.5,
			23,
			false,
		},
	};

	render::VelloRenderer renderer;
	for (auto const& scenario : scenarios)
	{
		Map map;
		QVERIFY2(map.loadFrom(scenario.file), qPrintable(scenario.file));
		auto const frame = mapFrame(
			map, { 512, 384 }, scenario.device_pixel_ratio,
			scenario.rotation_degrees, scenario.overprinting
		);
		QVERIFY(frame);
		auto const rendered = renderer.renderOffscreen(frame);
		QVERIFY2(rendered, renderer.lastError().c_str());
		auto const actual = imageFromVello(*rendered);
		auto const expected = referenceImage(*frame);
		QCOMPARE(actual.size(), expected.size());
		auto const difference = compareImages(actual, expected);
		qInfo() << scenario.file
		        << "DPR" << scenario.device_pixel_ratio
		        << "rotation" << scenario.rotation_degrees
		        << "overprint" << scenario.overprinting
		        << "mean delta" << difference.mean_channel_delta
		        << "high-delta pixels" << difference.high_delta_pixels;
		QVERIFY2(
			difference.mean_channel_delta < 3,
			qPrintable(QStringLiteral("Mean channel delta %1 for %2")
			           .arg(difference.mean_channel_delta).arg(scenario.file))
		);
		QVERIFY2(
			difference.high_delta_pixels < actual.width() * actual.height() / 50,
			qPrintable(QStringLiteral("%1 high-delta pixels for %2")
			           .arg(difference.high_delta_pixels).arg(scenario.file))
		);
	}
}

void VelloRendererTest::nativeSurfaceLifecyclePresentsCurrentFrame()
{
	Map map;
	QVERIFY(map.loadFrom(example(QStringLiteral("complete map.omap"))));
	QWidget host;
	host.setWindowFlag(Qt::WindowStaysOnTopHint);
	auto* layout = new QVBoxLayout(&host);
	layout->setContentsMargins(0, 0, 0, 0);
	presentation::VelloCanvas canvas;
	layout->addWidget(&canvas);
	host.resize(640, 480);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host, 15000));
	host.raise();
	host.activateWindow();
	QTRY_COMPARE(canvas.surfaceState().phase, presentation::SurfacePhase::Exposed);
	QTest::qWait(250);
	auto const first = mapFrame(map, canvas.size());
	QVERIFY(first);
	canvas.setFrame(first);
	auto const first_result = waitForTerminalResult(canvas, first->id);
	QVERIFY2(first_result, canvas.lastError().c_str());
	QVERIFY(first_result->completion.status == render::FrameStatus::Presented
	        || first_result->completion.status == render::FrameStatus::TargetUnavailable);
	QCOMPARE(first_result->revision, first->revision);
	QCOMPARE(first_result->surface_sequence, canvas.surfaceState().sequence);
#if defined(Q_OS_MACOS)
	QCOMPARE(first_result->backend, std::uint8_t(2));
#elif defined(Q_OS_WIN)
	QCOMPARE(first_result->backend, std::uint8_t(3));
#else
	QCOMPARE(first_result->backend, std::uint8_t(1));
#endif
	QCOMPARE(first_result->scene_count, std::uint32_t(1));
	QCOMPARE(canvas.encodedSceneCount(), std::size_t(1));

	host.hide();
	QTRY_COMPARE(canvas.surfaceState().phase, presentation::SurfacePhase::Hidden);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host, 15000));
	QTRY_COMPARE(canvas.surfaceState().phase, presentation::SurfacePhase::Exposed);
	auto const resumed = waitForTerminalResult(canvas, first->id);
	QVERIFY2(resumed, canvas.lastError().c_str());
	QVERIFY(resumed->completion.status == render::FrameStatus::Presented
	        || resumed->completion.status == render::FrameStatus::TargetUnavailable);
	QCOMPARE(resumed->surface_sequence, canvas.surfaceState().sequence);
}

QTEST_MAIN(VelloRendererTest)
