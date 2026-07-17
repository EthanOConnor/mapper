/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    Manual end-to-end Qt Canvas Painter presentation benchmark.
 */

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <vector>

#include <QApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QVBoxLayout>
#include <QWindow>

#include "global.h"
#include "core/map.h"
#include "presentation/qt_canvas.h"
#include "render/frame_pipeline.h"

using namespace OpenOrienteering;

namespace {

using Clock = std::chrono::steady_clock;
double milliseconds(Clock::duration duration);

class BenchmarkCanvas final : public presentation::QtCanvas
{
public:
	using presentation::QtCanvas::QtCanvas;
	double last_paint_ms = 0;

protected:
	void paint(QCanvasPainter* painter) override
	{
		auto const started = Clock::now();
		presentation::QtCanvas::paint(painter);
		last_paint_ms = milliseconds(Clock::now() - started);
	}
};

double milliseconds(Clock::duration duration)
{
	return std::chrono::duration<double, std::milli>(duration).count();
}

double percentile(std::vector<double> samples, double fraction)
{
	std::ranges::sort(samples);
	auto const index = std::min(samples.size() - 1,
	                            std::size_t(fraction * double(samples.size() - 1)));
	return samples[index];
}

render::FramePacketPtr mapFrame(Map& map, QSize viewport)
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
	transform.scale(scale, scale);
	transform.translate(-extent.center().x(), -extent.center().y());
	render::FramePlanner planner;
	return planner.plan(*snapshot, {
		{
			std::uint32_t(viewport.width()),
			std::uint32_t(viewport.height()),
			1,
			transform,
		},
		{ extent, scale, RenderConfig::Screen | RenderConfig::HelperSymbols, 1 },
		false,
	});
}

bool await(bool (*condition)(void*), void* context, int timeout_ms)
{
	QElapsedTimer timer;
	timer.start();
	while (!condition(context) && timer.elapsed() < timeout_ms)
	{
		QApplication::processEvents(QEventLoop::AllEvents, 5);
		QThread::msleep(1);
	}
	return condition(context);
}

}  // namespace

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
	if (app.arguments().size() < 2 || app.arguments().size() > 3)
		return 2;
	auto iterations = 200;
	if (app.arguments().size() == 3)
		iterations = app.arguments()[2].toInt();
	if (iterations < 2)
		return 2;

	Map map;
	if (!map.loadFrom(app.arguments()[1]))
		return 3;

	QWidget host;
	auto* layout = new QVBoxLayout(&host);
	layout->setContentsMargins(0, 0, 0, 0);
	BenchmarkCanvas canvas;
	layout->addWidget(&canvas);
	host.resize(1024, 768);
	host.show();
	auto exposed = [](void* value) {
		auto* widget = static_cast<QWidget*>(value);
		return widget->windowHandle() && widget->windowHandle()->isExposed();
	};
	if (!await(exposed, &host, 15000))
		return 4;

	auto base = mapFrame(map, canvas.size());
	if (!base)
		return 5;
	auto submitted = std::uint64_t(0);
	QObject::connect(&canvas, &QRhiWidget::frameSubmitted, &canvas, [&] { ++submitted; });
	auto await_submission = [&] (std::uint64_t previous) {
		auto changed = [](void* value) {
			auto const* pair = static_cast<const std::pair<const std::uint64_t*, std::uint64_t>*>(value);
			return *pair->first > pair->second;
		};
		auto context = std::pair { &submitted, previous };
		return await(changed, &context, 15000);
	};

	for (auto index = 0; index < 20; ++index)
	{
		auto frame = std::make_shared<render::FramePacket>(*base);
		frame->id += render::FrameId(index + 1);
		frame->view.world_to_viewport.translate(index % 2 ? 0.125 : -0.125, 0);
		auto const previous = submitted;
		canvas.setFrame(frame);
		if (!await_submission(previous))
			return 6;
	}

	std::vector<double> samples;
	std::vector<double> paint_samples;
	samples.reserve(std::size_t(iterations));
	paint_samples.reserve(std::size_t(iterations));
	for (auto index = 0; index < iterations; ++index)
	{
		auto frame = std::make_shared<render::FramePacket>(*base);
		frame->id += render::FrameId(index + 100);
		frame->view.world_to_viewport.translate(index % 2 ? 0.125 : -0.125, 0);
		auto const previous = submitted;
		auto const started = Clock::now();
		canvas.setFrame(std::move(frame));
		if (!await_submission(previous))
			return 7;
		samples.push_back(milliseconds(Clock::now() - started));
		paint_samples.push_back(canvas.last_paint_ms);
	}

	std::cout << "{\n"
	          << "  \"iterations\": " << iterations << ",\n"
	          << "  \"viewport\": [" << canvas.width() << ", " << canvas.height() << "],\n"
	          << "  \"passes\": " << base->vector_passes.size() << ",\n"
	          << "  \"resident_images\": " << canvas.residentImageCount() << ",\n"
	          << "  \"submit_to_frame_submitted_ms\": {\"p50\": "
	          << percentile(samples, 0.50) << ", \"p95\": " << percentile(samples, 0.95)
	          << ", \"max\": " << *std::ranges::max_element(samples) << "}\n"
	          << "  ,\"paint_callback_cpu_ms\": {\"p50\": "
	          << percentile(paint_samples, 0.50) << ", \"p95\": " << percentile(paint_samples, 0.95)
	          << ", \"max\": " << *std::ranges::max_element(paint_samples) << "}\n"
	          << "}\n";
	return 0;
}
