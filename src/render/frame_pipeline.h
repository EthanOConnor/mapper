/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_FRAME_PIPELINE_H
#define OPENORIENTEERING_FRAME_PIPELINE_H

#include <vector>

#include "render/frame_packet.h"
#include "render/render_snapshot.h"

namespace OpenOrienteering::render {

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

}  // namespace OpenOrienteering::render

#endif
