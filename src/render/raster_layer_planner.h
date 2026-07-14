/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_RASTER_LAYER_PLANNER_H
#define OPENORIENTEERING_RASTER_LAYER_PLANNER_H

#include <cstddef>
#include <memory>
#include <vector>

#include "render/frame_pipeline.h"

namespace OpenOrienteering {

class Map;
class MapView;

namespace render {

struct RasterLayerPlan
{
	std::vector<VectorPass> below_map;
	std::vector<VectorPass> above_map;
	bool complete = true;
	std::size_t newly_resident_images = 0;
};

/**
 * Records visible raster templates into stable retained image scenes.
 *
 * Source decoding and caching remain properties of each template source. This
 * planner snapshots ready immutable pixels, preserves template order, and
 * admits only bounded new image data into any one frame.
 */
class RasterLayerPlanner
{
public:
	RasterLayerPlanner();
	~RasterLayerPlanner();

	RasterLayerPlanner(const RasterLayerPlanner&) = delete;
	RasterLayerPlanner& operator=(const RasterLayerPlanner&) = delete;

	RasterLayerPlan plan(const Map& map,
	                     const MapView& view,
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
