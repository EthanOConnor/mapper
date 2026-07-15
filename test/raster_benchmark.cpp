/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    Manual end-to-end raster benchmark. It waits for exact visible GDAL
 *    tiles, records them into retained image scenes, then measures Vello GPU
 *    render and readback. The raster path is supplied as argv[1].
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <vector>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QThread>
#include <QTransform>

#include "global.h"
#include "core/map.h"
#include "core/map_view.h"
#include "gdal/gdal_template.h"
#include "gui/util_gui.h"
#include "render/frame_pipeline.h"
#include "render/qpainter_frame_renderer.h"
#include "render/qt_render_bridge.h"
#include "render/template_layer_planner.h"
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

struct ImageDifference
{
	double mean_channel_delta = 0;
	std::size_t high_delta_pixels = 0;
	std::array<double, 4> channel_delta {};
};

ImageDifference compareWithReference(
	const render::VelloImage& actual, const render::FramePacket& frame)
{
	QImage reference(int(actual.width), int(actual.height), QImage::Format_ARGB32_Premultiplied);
	reference.fill(Qt::white);
	QPainter painter(&reference);
	auto const completion = render::QPainterFrameRenderer().render(painter, frame);
	if (completion.status != render::FrameStatus::Presented)
		return { -1, 0, {} };
	painter.end();

	QImage rendered(
		actual.rgba8.data(), int(actual.width), int(actual.height), int(actual.width * 4),
		QImage::Format_RGBA8888
	);
	auto const capture_prefix = qEnvironmentVariable("MAPPER_RASTER_BENCHMARK_CAPTURE");
	if (!capture_prefix.isEmpty())
	{
		reference.save(capture_prefix + QStringLiteral("-qpainter.png"));
		rendered.copy().save(capture_prefix + QStringLiteral("-vello.png"));
	}
	std::uint64_t total_delta = 0;
	std::array<std::uint64_t, 4> channel_delta {};
	std::size_t high_delta_pixels = 0;
	for (int y = 0; y < reference.height(); ++y)
	{
		for (int x = 0; x < reference.width(); ++x)
		{
			auto const left = rendered.pixelColor(x, y);
			auto const right = reference.pixelColor(x, y);
			std::array<int, 4> const deltas {
				std::abs(left.red() - right.red()),
				std::abs(left.green() - right.green()),
				std::abs(left.blue() - right.blue()),
				std::abs(left.alpha() - right.alpha()),
			};
			auto const delta = deltas[0] + deltas[1] + deltas[2] + deltas[3];
			total_delta += std::uint64_t(delta);
			for (std::size_t channel = 0; channel < channel_delta.size(); ++channel)
				channel_delta[channel] += std::uint64_t(deltas[channel]);
			high_delta_pixels += delta > 160;
		}
	}
	auto const channel_count = double(reference.width()) * reference.height() * 4;
	auto const pixel_count = double(reference.width()) * reference.height();
	return {
		total_delta / channel_count,
		high_delta_pixels,
		{
			channel_delta[0] / pixel_count,
			channel_delta[1] / pixel_count,
			channel_delta[2] / pixel_count,
			channel_delta[3] / pixel_count,
		},
	};
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
	MapView view { &map };
	auto raster = std::make_unique<GdalTemplate>(app.arguments()[1], &map);
	auto* const source = raster.get();
	if (!source->loadTemplateFile() || !source->isTiledSource())
	{
		std::cerr << source->errorString().toStdString() << '\n';
		return 3;
	}
	source->setTemplateState(Template::Loaded);
	map.addTemplate(0, std::move(raster));
	map.setFirstFrontTemplate(1);
	view.setTemplateVisibility(source, { 1, true });

	auto const extent = source->calculateTemplateBoundingBox();
	if (!extent.isValid() || extent.isEmpty())
		return 4;
	auto const visible = QRectF(
		extent.center() - QPointF(width * 0.5, height * 0.5), QSizeF(width, height)
	).intersected(extent);
	if (visible.isEmpty())
		return 4;
	auto const view_zoom = 1.0 / Util::mmToPixelPhysical(1.0);
	map.getTemplate(0)->updateRenderContext({ visible, view_zoom });

	render::TemplateLayerPlanner template_planner;
	render::TemplateLayerPlan template_plan;
	QElapsedTimer ready_timer;
	ready_timer.start();
	do
	{
		QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
		template_plan = template_planner.plan(map, view, render::fromQRectF(visible), 1);
		if (template_plan.complete && template_plan.newly_resident_images == 0
		    && (!template_plan.below_map.empty() || !template_plan.above_map.empty()))
			break;
		QThread::msleep(1);
	} while (ready_timer.elapsed() < 30000);
	if (!template_plan.complete)
		return 5;

	auto const snapshot = map.publishRenderSnapshot();
	if (!snapshot)
		return 6;
	QTransform world_to_view;
	world_to_view.translate(width * 0.5, height * 0.5);
	world_to_view.translate(-visible.center().x(), -visible.center().y());
	render::FrameRequest request {
		{
			std::uint32_t(width), std::uint32_t(height), 1,
			render::fromQTransform(world_to_view),
		},
		{ render::fromQRectF(visible), 1, RenderConfig::Screen, 1 },
		false,
	};
	request.below_map = template_plan.below_map;
	request.above_map = template_plan.above_map;
	render::FramePlanner frame_planner;
	auto const base_frame = frame_planner.plan(*snapshot, request);

	render::VelloRenderer renderer;
	auto initial = renderer.renderOffscreen(base_frame);
	if (!initial)
	{
		std::cerr << renderer.lastError() << '\n';
		return 7;
	}
	auto const difference = compareWithReference(*initial, *base_frame);
	if (difference.mean_channel_delta < 0)
		return 7;
	std::vector<double> samples;
	samples.reserve(std::size_t(iterations));
	for (auto index = 0; index < iterations; ++index)
	{
		auto frame = std::make_shared<render::FramePacket>(*base_frame);
		frame->id = render::FrameId(index + 2);
		frame->view.world_to_viewport.dx += double(index % 5) / 8.0;
		auto const started = Clock::now();
		auto rendered = renderer.renderOffscreen(frame);
		auto const finished = Clock::now();
		if (!rendered)
		{
			std::cerr << renderer.lastError() << '\n';
			return 8;
		}
		samples.push_back(milliseconds(finished - started));
	}

	std::size_t raster_commands = 0;
	for (auto const& pass : request.below_map)
		raster_commands += pass.scene ? pass.scene->commands.size() : 0;
	for (auto const& pass : request.above_map)
		raster_commands += pass.scene ? pass.scene->commands.size() : 0;
	std::cout << "{\n"
	          << "  \"iterations\": " << iterations << ",\n"
	          << "  \"width\": " << width << ",\n"
	          << "  \"height\": " << height << ",\n"
	          << "  \"source_width\": " << source->getRasterPixelSize().width() << ",\n"
	          << "  \"source_height\": " << source->getRasterPixelSize().height() << ",\n"
	          << "  \"exact_ready_ms\": " << ready_timer.elapsed() << ",\n"
	          << "  \"raster_commands\": " << raster_commands << ",\n"
	          << "  \"retained_images\": " << renderer.cachedImageCount() << ",\n"
	          << "  \"reference_difference\": {"
	          << "\"mean_channel_delta\": " << difference.mean_channel_delta
	          << ", \"high_delta_pixels\": " << difference.high_delta_pixels
	          << ", \"mean_rgba_delta\": [" << difference.channel_delta[0]
	          << ", " << difference.channel_delta[1]
	          << ", " << difference.channel_delta[2]
	          << ", " << difference.channel_delta[3] << "]"
	          << "},\n"
	          << "  \"gpu_render_and_readback\": {"
	          << "\"p50_ms\": " << percentile(samples, 0.50)
	          << ", \"p95_ms\": " << percentile(samples, 0.95)
	          << ", \"max_ms\": " << *std::ranges::max_element(samples)
	          << "}\n}\n";
	auto const pixels = std::size_t(width) * std::size_t(height);
	// Qt and Vello use different maintained bilinear samplers. The narrow
	// photographic tolerance covers that expected subpixel difference; the
	// fractional transparent/opaque seam fixtures enforce tile continuity.
	return difference.mean_channel_delta < 3.25
	       && difference.high_delta_pixels * 50 < pixels ? 0 : 9;
}
