/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_FRAME_PIPELINE_H
#define OPENORIENTEERING_FRAME_PIPELINE_H

#include <cstddef>
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

/**
 * The map pass is recorded as one scene per span of at most this many objects
 * of one color. Spans whose objects are unchanged between revisions keep their
 * scene identity, so backends reuse the retained encodings and only the spans
 * containing edited objects are re-recorded and re-encoded.
 */
inline constexpr std::size_t frame_chunk_span_size = 1024;

/** Produces monotonically identified immutable frames from published state. */
class FramePlanner
{
public:
	/** Plans a frame which has no document map pass (for example an empty-map help view). */
	FramePacketPtr plan(const FrameRequest& request);
	FramePacketPtr plan(const MapRenderSnapshot& snapshot, const FrameRequest& request);
	/** Identifies a camera-only presentation over the last completed content generation. */
	FramePacketPtr cameraFrame(FrameView view, Revision content_revision);

private:
	struct CachedChunk
	{
		std::shared_ptr<const RenderIR> scene;
		SnapshotObjectIds ids;
		std::size_t begin = 0;
		std::size_t end = 0;
		int priority = 0;
		Color draw_color;
		Revision recorded_revision = 0;
	};

	void planMapPasses(const MapRenderSnapshot& snapshot, const FrameRequest& request);

	FrameId next_frame_id_ = 1;
	std::uint64_t cached_snapshot_identity_ = 0;
	Revision cached_revision_ = 0;
	RenderRequest cached_request_;
	bool cached_overprinting_ = false;
	std::vector<VectorPass> cached_map_passes_;
	std::vector<CachedChunk> cached_chunks_;
};

}  // namespace OpenOrienteering::render

#endif
