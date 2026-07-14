/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    Manual full-GPU Vello benchmark. Every timed sample renders the retained
 *    scene, waits for the GPU, and reads RGBA pixels back to the CPU.
 */

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

#include <QApplication>
#include <QTransform>

#include "global.h"
#include "core/map.h"
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

}  // namespace

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
	if (app.arguments().size() < 2 || app.arguments().size() > 5)
		return 2;

	bool valid = true;
	auto const iterations = app.arguments().value(2, QStringLiteral("60")).toInt(&valid);
	if (!valid || iterations < 2)
		return 2;
	auto const width = app.arguments().value(3, QStringLiteral("1024")).toInt(&valid);
	if (!valid || width < 1 || width > 8192)
		return 2;
	auto const height = app.arguments().value(4, QStringLiteral("768")).toInt(&valid);
	if (!valid || height < 1 || height > 8192)
		return 2;

	Map map;
	if (!map.loadFrom(app.arguments()[1]))
		return 3;
	auto const snapshot = map.publishRenderSnapshot();
	if (!snapshot)
		return 4;
	auto const extent = map.calculateExtent(true).toAlignedRect().adjusted(-1, -1, 1, 1);
	auto const scale = std::min(
		(width - 40.0) / extent.width(),
		(height - 40.0) / extent.height()
	);
	QTransform transform;
	transform.translate(width / 2.0, height / 2.0);
	transform.scale(scale, scale);
	transform.translate(-extent.center().x(), -extent.center().y());
	render::FramePlanner planner;
	auto const base_frame = planner.plan(*snapshot, {
		{
			std::uint32_t(width),
			std::uint32_t(height),
			1,
			render::fromQTransform(transform),
		},
		{
			render::fromQRectF(extent),
			scale,
			RenderConfig::Screen | RenderConfig::HelperSymbols,
			1,
		},
		false,
	});

	render::VelloRenderer renderer;
	if (!renderer.renderOffscreen(base_frame))
	{
		std::cerr << renderer.lastError() << '\n';
		return 5;
	}
	std::vector<double> samples;
	samples.reserve(std::size_t(iterations));
	for (auto i = 0; i < iterations; ++i)
	{
		auto frame = std::make_shared<render::FramePacket>(*base_frame);
		frame->id = render::FrameId(i + 2);
		frame->view.world_to_viewport.dx += double(i % 5) / 8.0;
		auto const started = Clock::now();
		auto rendered = renderer.renderOffscreen(frame);
		auto const finished = Clock::now();
		if (!rendered)
		{
			std::cerr << renderer.lastError() << '\n';
			return 6;
		}
		samples.push_back(milliseconds(finished - started));
	}

	auto const command_count = base_frame->vector_passes.empty()
	                         ? 0
	                         : base_frame->vector_passes.front().scene->commands.size();
	std::cout << "{\n"
	          << "  \"iterations\": " << iterations << ",\n"
	          << "  \"width\": " << width << ",\n"
	          << "  \"height\": " << height << ",\n"
	          << "  \"commands\": " << command_count << ",\n"
	          << "  \"encoded_scenes\": " << renderer.encodedSceneCount() << ",\n"
	          << "  \"gpu_render_and_readback\": {"
	          << "\"p50_ms\": " << percentile(samples, 0.50)
	          << ", \"p95_ms\": " << percentile(samples, 0.95)
	          << ", \"max_ms\": " << *std::ranges::max_element(samples)
	          << "}\n}\n";
	return renderer.encodedSceneCount() == 1 ? 0 : 7;
}
