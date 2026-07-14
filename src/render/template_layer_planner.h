/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_TEMPLATE_LAYER_PLANNER_H
#define OPENORIENTEERING_TEMPLATE_LAYER_PLANNER_H

#include <cstddef>
#include <memory>
#include <vector>

#include "render/frame_pipeline.h"

namespace OpenOrienteering {

class Map;
class MapView;

namespace render {

struct TemplateLayerPlan
{
	std::vector<VectorPass> below_map;
	std::vector<VectorPass> above_map;
	bool complete = true;
	std::size_t newly_resident_images = 0;
};

/**
 * Records every visible template into ordered retained scenes.
 *
 * Source decoding and caching remain properties of each template source. This
 * planner snapshots ready immutable pixels, preserves template order, and
 * admits only bounded new image data into any one frame.
 */
class TemplateLayerPlanner
{
public:
	TemplateLayerPlanner();
	~TemplateLayerPlanner();

	TemplateLayerPlanner(const TemplateLayerPlanner&) = delete;
	TemplateLayerPlanner& operator=(const TemplateLayerPlanner&) = delete;

	TemplateLayerPlan plan(const Map& map,
	                     const MapView& view,
	                     Rect visible_map_rect,
	                     double view_scale,
	                     bool on_screen = true);
	TemplateLayerPlan plan(const Map& map,
	                     const MapView* view,
	                     Rect visible_map_rect,
	                     double view_scale,
	                     bool on_screen = true);

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace render
}  // namespace OpenOrienteering

#endif
