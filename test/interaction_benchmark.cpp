/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    Manual end-to-end interaction benchmark. Times the full CPU+GPU cost of
 *    the three interaction paths which invalidate retained render state:
 *    a camera-only change (retained scene), an object edit (new revision),
 *    and a zoom step (new render request). Each timed sample runs
 *    publish + plan + offscreen render, so scene re-encoding is included.
 */

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

#include <QApplication>
#include <QTransform>

#include "global.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "render/frame_pipeline.h"
#include "render/qt_render_bridge.h"
#include "render/vello_renderer.h"

using namespace OpenOrienteering;

namespace {

using Clock = std::chrono::steady_clock;

double milliseconds(Clock::duration duration)
{
	return std::chrono::duration<double, std::milli>(duration).count();
}

double percentile(std::vector<double> samples, double fraction)
{
	std::ranges::sort(samples);
	auto const index = std::min(
		samples.size() - 1,
		std::size_t(fraction * double(samples.size() - 1))
	);
	return samples[index];
}

void printSeries(const char* name, const std::vector<double>& samples)
{
	std::cout << "  \"" << name << "\": {\"p50_ms\": " << percentile(samples, 0.50)
	          << ", \"p95_ms\": " << percentile(samples, 0.95)
	          << ", \"max_ms\": " << *std::ranges::max_element(samples) << "}";
}

}  // namespace

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
	if (app.arguments().size() < 2 || app.arguments().size() > 5)
		return 2;

	bool valid = true;
	auto const iterations = app.arguments().value(2, QStringLiteral("30")).toInt(&valid);
	if (!valid || iterations < 2)
		return 2;
	auto const width = app.arguments().value(3, QStringLiteral("2560")).toInt(&valid);
	if (!valid || width < 1 || width > 8192)
		return 2;
	auto const height = app.arguments().value(4, QStringLiteral("1440")).toInt(&valid);
	if (!valid || height < 1 || height > 8192)
		return 2;

	auto const load_started = Clock::now();
	Map map;
	if (!map.loadFrom(app.arguments()[1]))
		return 3;
	auto const load_finished = Clock::now();
	auto* object = map.getPart(0)->getObject(0);
	if (!object)
		return 4;

	auto const extent = map.calculateExtent(true).toAlignedRect().adjusted(-1, -1, 1, 1);
	auto const scale = std::min(
		(width - 40.0) / extent.width(),
		(height - 40.0) / extent.height()
	);
	auto makeView = [&](double zoom) {
		QTransform transform;
		transform.translate(width / 2.0, height / 2.0);
		transform.scale(scale * zoom, scale * zoom);
		transform.translate(-extent.center().x(), -extent.center().y());
		return render::FrameView {
			std::uint32_t(width),
			std::uint32_t(height),
			1,
			render::fromQTransform(transform),
		};
	};
	auto makeRequest = [&](double zoom) {
		return render::RenderRequest {
			render::fromQRectF(extent),
			scale * zoom,
			RenderConfig::Screen | RenderConfig::HelperSymbols,
			1,
		};
	};

	render::FramePlanner planner;
	render::VelloRenderer renderer;
	auto renderOnce = [&](double zoom) -> bool {
		auto frame = planner.plan(*map.publishRenderSnapshot(), {
			makeView(zoom),
			makeRequest(zoom),
			false,
		});
		if (!renderer.renderOffscreen(frame))
		{
			std::cerr << renderer.lastError() << '\n';
			return false;
		}
		return true;
	};
	if (!renderOnce(1))
		return 5;

	// Camera-only samples: the scene is retained, only the view changes.
	std::vector<double> camera_samples;
	camera_samples.reserve(std::size_t(iterations));
	for (auto i = 0; i < iterations; ++i)
	{
		auto const planned = planner.plan(*map.publishRenderSnapshot(), {
			makeView(1),
			makeRequest(1),
			false,
		});
		auto frame = std::make_shared<render::FramePacket>(*planned);
		frame->view.world_to_viewport.dx += double(i % 5) / 8.0;
		auto const started = Clock::now();
		if (!renderer.renderOffscreen(frame))
			return 6;
		camera_samples.push_back(milliseconds(Clock::now() - started));
	}

	// Edit samples: each moves one object, so revision and scene change.
	std::vector<double> edit_samples;
	edit_samples.reserve(std::size_t(iterations));
	for (auto i = 0; i < iterations; ++i)
	{
		object->move(i % 2 == 0 ? 1 : -1, 0);
		auto const started = Clock::now();
		if (!renderOnce(1))
			return 7;
		edit_samples.push_back(milliseconds(Clock::now() - started));
	}

	// Zoom samples: each uses a different scaling, so the request changes.
	std::vector<double> zoom_samples;
	zoom_samples.reserve(std::size_t(iterations));
	for (auto i = 0; i < iterations; ++i)
	{
		auto const zoom = 1.0 + 0.01 * (i + 1);
		auto const started = Clock::now();
		if (!renderOnce(zoom))
			return 8;
		zoom_samples.push_back(milliseconds(Clock::now() - started));
	}

	std::cout << "{\n"
	          << "  \"iterations\": " << iterations << ",\n"
	          << "  \"width\": " << width << ",\n"
	          << "  \"height\": " << height << ",\n"
	          << "  \"load_ms\": " << milliseconds(load_finished - load_started) << ",\n"
	          << "  \"encoded_scenes\": " << renderer.encodedSceneCount() << ",\n";
	printSeries("camera_only", camera_samples);
	std::cout << ",\n";
	printSeries("edit_frame", edit_samples);
	std::cout << ",\n";
	printSeries("zoom_step_frame", zoom_samples);
	std::cout << "\n}\n";
	return 0;
}
