/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    Manual publication benchmark for the immutable render pipeline.
 */

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

#include <QApplication>

#include "global.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "render/qt_render_bridge.h"
#include "render/render_snapshot.h"

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
	auto const index = std::min(samples.size() - 1,
	                            std::size_t(fraction * double(samples.size() - 1)));
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
	if (app.arguments().size() < 2 || app.arguments().size() > 3)
		return 2;

	bool valid_iterations = true;
	auto iterations = 200;
	if (app.arguments().size() == 3)
		iterations = app.arguments()[2].toInt(&valid_iterations);
	if (!valid_iterations || iterations < 2)
		return 2;

	Map map;
	if (!map.loadFrom(app.arguments()[1]))
		return 3;
	auto* object = map.getPart(0)->getObject(0);
	if (!object)
		return 4;

	auto const request = render::RenderRequest {
		render::fromQRectF(map.calculateExtent(true)),
		10,
		RenderConfig::HelperSymbols,
		1,
	};
	map.publishRenderSnapshot()->buildIR(request);

	std::vector<double> edit_and_publish;
	std::vector<double> publish;
	std::vector<double> record_ir;
	edit_and_publish.reserve(iterations);
	publish.reserve(iterations);
	record_ir.reserve(iterations);
	std::size_t object_count = 0;
	std::size_t command_count = 0;

	for (auto i = 0; i < iterations; ++i)
	{
		auto const started = Clock::now();
		object->move(i % 2 == 0 ? 1 : -1, 0);
		auto const edited = Clock::now();
		auto snapshot = map.publishRenderSnapshot();
		auto const published = Clock::now();
		auto ir = snapshot->buildIR(request);
		auto const recorded = Clock::now();

		edit_and_publish.push_back(milliseconds(published - started));
		publish.push_back(milliseconds(published - edited));
		record_ir.push_back(milliseconds(recorded - published));
		object_count = snapshot->objectCount();
		command_count = ir->commands.size();
	}

	std::cout << "{\n  \"iterations\": " << iterations
	          << ",\n  \"objects\": " << object_count
	          << ",\n  \"commands\": " << command_count << ",\n";
	printSeries("edit_and_publish", edit_and_publish);
	std::cout << ",\n";
	printSeries("publish_only", publish);
	std::cout << ",\n";
	printSeries("record_ir", record_ir);
	std::cout << "\n}\n";
	return 0;
}
