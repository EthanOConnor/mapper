/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    Manual diagnostic: renders a map offscreen at the given size and saves
 *    the pixels to a PNG for visual verification.
 */

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

#include <QImage>
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
	if (app.arguments().size() < 2 || app.arguments().size() > 6)
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
	auto const rendered = renderer.renderOffscreen(base_frame);
	if (!rendered)
	{
		std::cerr << renderer.lastError() << '\n';
		return 5;
	}
	QImage image(int(rendered->width), int(rendered->height), QImage::Format_RGBA8888);
	for (int y = 0; y < image.height(); ++y)
	{
		std::memcpy(image.scanLine(y),
		            rendered->rgba8.data() + std::size_t(y) * rendered->width * 4,
		            std::size_t(rendered->width) * 4);
	}
	auto const out = app.arguments().size() == 6
	               ? app.arguments()[5]
	               : QStringLiteral("offscreen_snapshot.png");
	image.save(out);
	std::cout << "{\"width\": " << rendered->width
	          << ", \"height\": " << rendered->height
	          << ", \"passes\": " << base_frame->vector_passes.size()
	          << ", \"saved\": \"" << out.toStdString() << "\"}\n";
	return 0;
}
