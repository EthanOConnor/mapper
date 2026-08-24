/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    Manual diagnostic: loads a map into a real MapWidget + VelloCanvas
 *    window, waits for presentation, and reports what the canvas holds.
 */

#include <iostream>

#include <QApplication>
#include <QTest>
#include <QWidget>

#include "global.h"
#include "core/map.h"
#include "core/map_view.h"
#include "gui/map/map_widget.h"
#include "presentation/vello_canvas.h"

using namespace OpenOrienteering;

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
	if (app.arguments().size() != 2)
		return 2;

	Map map;
	if (!map.loadFrom(app.arguments()[1]))
	{
		std::cout << "{\"error\": \"load failed\"}\n";
		return 3;
	}
	MapView view(nullptr, &map);
	MapWidget widget(false);
	widget.resize(1200, 800);
	widget.setMapView(&view);
	widget.adjustViewToRect(map.calculateExtent(true), MapWidget::ContinuousZoom);
	auto* canvas = dynamic_cast<presentation::VelloCanvas*>(
		widget.findChild<QWidget*>(QStringLiteral("mapVelloCanvas")));
	if (!canvas)
		return 4;
	widget.show();
	QTest::qWaitForWindowExposed(&widget);
	widget.raise();
	widget.activateWindow();
	QTest::qWait(3000);

	auto report = [&](const char* stage) {
		auto const& frame = canvas->currentFrame();
		std::size_t passes = 0;
		std::size_t commands = 0;
		if (frame)
		{
			passes = frame->vector_passes.size();
			for (auto const& pass : frame->vector_passes)
			{
				if (pass.scene)
					commands += pass.scene->commands.size();
			}
		}
		std::cout << "{\"stage\": \"" << stage << '"'
		          << ", \"have_frame\": " << (frame ? "true" : "false")
		          << ", \"frame_id\": " << (frame ? frame->id : 0)
		          << ", \"frame_revision\": " << (frame ? frame->revision : 0)
		          << ", \"vector_passes\": " << passes
		          << ", \"commands\": " << commands
		          << ", \"encoded_scenes\": " << canvas->encodedSceneCount();
		auto const& result = canvas->lastResult();
		if (result)
		{
			std::cout << ", \"last_status\": " << int(result->completion.status)
			          << ", \"last_result_frame\": " << result->completion.frame_id
			          << ", \"scene_count\": " << result->scene_count;
		}
		else
		{
			std::cout << ", \"last_status\": null";
		}
		std::cout << ", \"last_error\": \"" << canvas->lastError() << "\"}\n";
	};

	std::cout << "{\"map_objects\": " << map.getNumObjects()
	          << ", \"map_parts\": " << map.getNumParts()
	          << ", \"map_colors\": " << map.getNumColors() << "}\n";
	report("initial");

	// Mirror the "Show whole map" action, then wait for the refresh.
	widget.adjustViewToRect(map.calculateExtent(true), MapWidget::ContinuousZoom);
	QTest::qWait(2000);
	report("whole_map");

	view.setZoom(view.getZoom() * 2);
	QTest::qWait(2000);
	report("zoom_in_2x");
	return 0;
}
