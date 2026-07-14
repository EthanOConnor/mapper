/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_FRAME_PIPELINE_H
#define OPENORIENTEERING_FRAME_PIPELINE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "render/render_snapshot.h"

namespace OpenOrienteering::render {

using FrameId = std::uint64_t;

/** The logical viewport and camera shared by every backend for one frame. */
struct FrameView
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	double device_pixel_ratio = 1;
	Transform world_to_viewport;
};

/** One ordered vector contribution to a frame. */
struct VectorPass
{
	enum class Space : std::uint8_t
	{
		World,
		Viewport,
	};

	std::shared_ptr<const RenderIR> scene;
	BlendMode blend = BlendMode::SourceOver;
	double opacity = 1;
	/** Render to transparent intermediate storage before compositing. */
	bool isolated = false;
	Space space = Space::World;
};

/**
 * A complete immutable backend input.
 *
 * Every resource needed to draw the frame is owned by this packet or by the
 * immutable data it shares. A backend never reads a live Map.
 */
class FramePacket
{
public:
	FrameId id = 0;
	Revision revision = 0;
	FrameView view;
	RenderRequest render_request;
	std::vector<VectorPass> vector_passes;
	bool raster_complete = true;
};

using FramePacketPtr = std::shared_ptr<const FramePacket>;

struct FrameRequest
{
	FrameRequest() = default;
	FrameRequest(FrameView view, RenderRequest render, bool simulate_overprinting = false)
	 : view(view)
	 , render(render)
	 , simulate_overprinting(simulate_overprinting)
	{}

	FrameView view;
	RenderRequest render;
	bool simulate_overprinting = false;
	std::vector<VectorPass> below_map;
	std::vector<VectorPass> above_map;
	bool raster_complete = true;
};

/** Produces monotonically identified immutable frames from published state. */
class FramePlanner
{
public:
	/** Plans a frame which has no document map pass (for example an empty-map help view). */
	FramePacketPtr plan(const FrameRequest& request);
	FramePacketPtr plan(const MapRenderSnapshot& snapshot, const FrameRequest& request);

private:
	FrameId next_frame_id_ = 1;
	std::uint64_t cached_snapshot_identity_ = 0;
	Revision cached_revision_ = 0;
	RenderRequest cached_request_;
	bool cached_overprinting_ = false;
	std::vector<VectorPass> cached_map_passes_;
};

enum class FrameStatus : std::uint8_t
{
	Presented,
	TargetUnavailable,
	DroppedStale,
	SurfaceLost,
	Failed,
};

/** A completion always identifies the submission to which it belongs. */
struct FrameCompletion
{
	FrameId frame_id = 0;
	FrameStatus status = FrameStatus::TargetUnavailable;
};

}  // namespace OpenOrienteering::render

#endif
