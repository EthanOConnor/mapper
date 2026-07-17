/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "qt_canvas_renderer_t.h"

#include <memory>

#include <QtTest>
#include <QCanvasPath>
#include <QImage>
#include <QPainterPath>
#include <QVBoxLayout>

#include "global.h"
#include "presentation/qt_canvas.h"
#include "render/qt_canvas_renderer.h"
#include "render/qt_render_scene.h"

using namespace OpenOrienteering;

namespace {

render::PathPtr rectangle(QRectF bounds)
{
	QPainterPath path;
	path.addRect(bounds);
	return render::sharePainterPath(std::move(path));
}

render::FramePacketPtr operationFrame(render::FrameId id, bool multiply = false,
	                                   render::ImagePtr image = {})
{
	render::QtRenderSceneBuilder builder(42, { 0, 0, 128, 128 });
	builder.fillPath(rectangle({ 0, 0, 128, 128 }), Qt::white);
	builder.fillPath(rectangle({ 12, 12, 48, 36 }), QColor(220, 30, 40));
	builder.strokePath(
		rectangle({ 72, 16, 36, 36 }),
		QPen(QColor(20, 40, 220), 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
	);
	if (image)
		builder.drawImage(std::move(image), { 76, 72, 32, 32 });

	auto frame = std::make_shared<render::FramePacket>();
	frame->id = id;
	frame->revision = 42;
	frame->view = { 128, 128, 1, {} };
	frame->vector_passes.push_back({
		builder.finish(),
		multiply ? QPainter::CompositionMode_Multiply
		         : QPainter::CompositionMode_SourceOver,
		1,
		multiply,
	});
	return frame;
}

void showAndPresent(presentation::QtCanvas& canvas,
	                const render::FramePacketPtr& frame)
{
	canvas.setFrame(frame);
	QTRY_VERIFY_WITH_TIMEOUT(
		canvas.lastCompletion()
		&& canvas.lastCompletion()->frame_id == frame->id
		&& canvas.lastCompletion()->status == render::FrameStatus::Presented,
		15000
	);
}

}  // namespace

void QtCanvasRendererTest::initTestCase()
{
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
}

void QtCanvasRendererTest::scenePublishesDirectCanvasGeometry()
{
	auto path = rectangle({ 1, 2, 30, 40 });
	QCOMPARE(path->painterPath().controlPointRect(), QRectF(1, 2, 30, 40));
	QVERIFY(path->canvasPath().commandsSize() > 0);
	QVERIFY(path->canvasPath().commandsDataSize() > 0);

	render::QtRenderSceneBuilder builder(7);
	builder.fillPath(path, Qt::black);
	auto frame = std::make_shared<render::FramePacket>();
	frame->id = 1;
	frame->vector_passes.push_back({ builder.finish() });
	QVERIFY(!render::QtCanvasRenderer::requiresSoftwareFrame(*frame));
}

void QtCanvasRendererTest::multiplySelectsSoftwareFallback()
{
	QVERIFY(render::QtCanvasRenderer::requiresSoftwareFrame(*operationFrame(2, true)));
	QVERIFY(!render::QtCanvasRenderer::requiresSoftwareFrame(*operationFrame(3, false)));

	render::QtRenderSceneBuilder builder(8);
	builder.pushLayer(0.5, QPainter::CompositionMode_Multiply);
	builder.fillPath(rectangle({ 0, 0, 10, 10 }), Qt::black);
	builder.popLayer();
	auto frame = std::make_shared<render::FramePacket>();
	frame->vector_passes.push_back({ builder.finish() });
	QVERIFY(render::QtCanvasRenderer::requiresSoftwareFrame(*frame));
}

void QtCanvasRendererTest::widgetPresentsCurrentFrame()
{
	QWidget host;
	auto* layout = new QVBoxLayout(&host);
	layout->setContentsMargins(0, 0, 0, 0);
	presentation::QtCanvas canvas;
	layout->addWidget(&canvas);
	host.resize(128, 128);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host, 15000));

	auto frame = operationFrame(4);
	showAndPresent(canvas, frame);
	QVERIFY(!canvas.usedSoftwareFallback());
	QCOMPARE(canvas.currentFrame(), frame);

	QTest::qWait(100);
	auto image = canvas.grabFramebuffer();
	QVERIFY(!image.isNull());
	QVERIFY(image.pixelColor(image.width() / 5, image.height() / 5).red() > 180);
}

void QtCanvasRendererTest::transformedStrokeWidthIsNotDoubleScaled()
{
	QWidget host;
	auto* layout = new QVBoxLayout(&host);
	layout->setContentsMargins(0, 0, 0, 0);
	presentation::QtCanvas canvas;
	layout->addWidget(&canvas);
	host.resize(128, 128);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host, 15000));

	QPainterPath line;
	line.moveTo(5, 16);
	line.lineTo(27, 16);
	render::QtRenderSceneBuilder builder(43, { 0, 0, 32, 32 });
	builder.strokePath(
		render::sharePainterPath(std::move(line)),
		QPen(Qt::black, 2, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin)
	);
	auto frame = std::make_shared<render::FramePacket>();
	frame->id = 8;
	frame->revision = 43;
	frame->view = { 128, 128, 1, QTransform::fromScale(4, 4) };
	frame->vector_passes.push_back({ builder.finish() });
	showAndPresent(canvas, frame);

	QTest::qWait(100);
	auto image = canvas.grabFramebuffer();
	QVERIFY(!image.isNull());
	auto sample = [&](int x, int y) {
		return image.pixelColor(
			x * image.width() / canvas.width(),
			y * image.height() / canvas.height()
		);
	};
	QVERIFY(sample(64, 64).lightness() < 32);
	QVERIFY(sample(64, 50).lightness() > 224);
	QVERIFY(sample(64, 78).lightness() > 224);
}

void QtCanvasRendererTest::transformedFillAntialiasIsDeviceSized()
{
	QWidget host;
	auto* layout = new QVBoxLayout(&host);
	layout->setContentsMargins(0, 0, 0, 0);
	presentation::QtCanvas canvas;
	layout->addWidget(&canvas);
	host.resize(128, 128);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host, 15000));

	render::QtRenderSceneBuilder builder(44, { 0, 0, 16, 16 });
	builder.fillPath(rectangle({ 2, 5, 12, 6 }), Qt::black);
	auto frame = std::make_shared<render::FramePacket>();
	frame->id = 9;
	frame->revision = 44;
	frame->view = { 128, 128, 1, QTransform::fromScale(8, 8) };
	frame->vector_passes.push_back({ builder.finish() });
	showAndPresent(canvas, frame);

	QTest::qWait(100);
	auto image = canvas.grabFramebuffer();
	QVERIFY(!image.isNull());
	auto sample = [&](int x, int y) {
		return image.pixelColor(
			x * image.width() / canvas.width(),
			y * image.height() / canvas.height()
		);
	};
	QVERIFY(sample(64, 64).lightness() < 32);
	QVERIFY(sample(64, 35).lightness() > 224);
	QVERIFY(sample(64, 93).lightness() > 224);
}

void QtCanvasRendererTest::linePatternDoesNotLeakStencilClip()
{
	QWidget host;
	auto* layout = new QVBoxLayout(&host);
	layout->setContentsMargins(0, 0, 0, 0);
	presentation::QtCanvas canvas;
	layout->addWidget(&canvas);
	host.resize(128, 128);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host, 15000));

	render::QtRenderSceneBuilder builder(45, { 0, 0, 128, 128 });
	builder.fillPath(rectangle({ 0, 0, 128, 128 }), Qt::white);
	builder.drawLinePattern(rectangle({ 0, 0, 48, 128 }), Qt::blue,
	                        0, 12, 2, 4);
	builder.fillPath(rectangle({ 80, 40, 32, 48 }), Qt::black);
	auto frame = std::make_shared<render::FramePacket>();
	frame->id = 10;
	frame->revision = 45;
	frame->view = { 128, 128, 1, {} };
	frame->vector_passes.push_back({ builder.finish() });
	showAndPresent(canvas, frame);

	QTest::qWait(100);
	auto image = canvas.grabFramebuffer();
	QVERIFY(!image.isNull());
	QVERIFY(image.pixelColor(image.width() * 3 / 4, image.height() / 2).lightness() < 32);
}

void QtCanvasRendererTest::sceneClipPopRestoresStencilState()
{
	QWidget host;
	auto* layout = new QVBoxLayout(&host);
	layout->setContentsMargins(0, 0, 0, 0);
	presentation::QtCanvas canvas;
	layout->addWidget(&canvas);
	host.resize(128, 128);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host, 15000));

	render::QtRenderSceneBuilder builder(46, { 0, 0, 128, 128 });
	builder.fillPath(rectangle({ 0, 0, 128, 128 }), Qt::white);
	builder.pushClip(rectangle({ 0, 0, 96, 128 }));
	builder.fillPath(rectangle({ 0, 0, 128, 128 }), Qt::red);
	builder.pushClip(rectangle({ 0, 0, 48, 128 }));
	builder.fillPath(rectangle({ 0, 0, 128, 128 }), Qt::blue);
	builder.popClip();
	builder.fillPath(rectangle({ 64, 40, 16, 48 }), Qt::green);
	builder.popClip();
	builder.fillPath(rectangle({ 104, 40, 16, 48 }), Qt::black);
	auto frame = std::make_shared<render::FramePacket>();
	frame->id = 11;
	frame->revision = 46;
	frame->view = { 128, 128, 1, {} };
	frame->vector_passes.push_back({ builder.finish() });
	showAndPresent(canvas, frame);

	QTest::qWait(100);
	auto image = canvas.grabFramebuffer();
	QVERIFY(!image.isNull());
	QVERIFY(image.pixelColor(image.width() / 4, image.height() / 2).blue() > 224);
	QVERIFY(image.pixelColor(image.width() * 9 / 16, image.height() / 2).green() > 96);
	QVERIFY(image.pixelColor(image.width() * 7 / 8, image.height() / 2).lightness() < 32);
}

void QtCanvasRendererTest::retainedImagesStayResident()
{
	QImage pixels(2, 2, QImage::Format_RGBA8888);
	pixels.fill(QColor(30, 210, 60));
	auto image = std::make_shared<const QImage>(std::move(pixels));

	QWidget host;
	auto* layout = new QVBoxLayout(&host);
	layout->setContentsMargins(0, 0, 0, 0);
	presentation::QtCanvas canvas;
	layout->addWidget(&canvas);
	host.resize(128, 128);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host, 15000));

	auto first = operationFrame(5, false, image);
	showAndPresent(canvas, first);
	QCOMPARE(canvas.residentImageCount(), std::size_t(1));

	auto second = std::make_shared<render::FramePacket>(*first);
	second->id = 6;
	second->view.world_to_viewport.translate(3, 0);
	showAndPresent(canvas, second);
	QCOMPARE(canvas.residentImageCount(), std::size_t(1));

	showAndPresent(canvas, operationFrame(7, true));
	QVERIFY(canvas.usedSoftwareFallback());
}

QTEST_MAIN(QtCanvasRendererTest)
