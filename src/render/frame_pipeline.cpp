/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/frame_pipeline.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

#include <QtGlobal>

namespace OpenOrienteering::render {

namespace {

#ifndef MAPPER_OVERPRINTING_CORRECTION
#define MAPPER_OVERPRINTING_CORRECTION 2
#endif

void appendNormalFrame(std::vector<VectorPass>& passes,
                       const MapRenderSnapshot& snapshot,
                       const RenderRequest& request)
{
	passes.push_back({ snapshot.buildIR(request) });
}

constexpr double overprintingCorrectionOpacity()
{
#if MAPPER_OVERPRINTING_CORRECTION == 1
	return 1.0 / 8.0;
#elif MAPPER_OVERPRINTING_CORRECTION == 2
	return 1.0 / 4.0;
#else
	return 1.0 / 2.0;
#endif
}

bool sameRequest(const RenderRequest& left, const RenderRequest& right)
{
	auto const& a = left.bounding_box;
	auto const& b = right.bounding_box;
	return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height
	       && left.scaling == right.scaling
	       && left.options == right.options
	       && left.opacity == right.opacity;
}

bool spanBlocksUnchanged(const MapRenderSnapshot& snapshot,
                         std::span<const ObjectId> ids,
                         Revision recorded_revision)
{
	auto last_block = std::numeric_limits<std::size_t>::max();
	for (auto id : ids)
	{
		auto const block = std::size_t((id - 1) / snapshot_object_block_size);
		if (block == last_block)
			continue;
		if (snapshot.blockRevision(block) > recorded_revision)
			return false;
		last_block = block;
	}
	return true;
}

void appendOverprintingFrame(std::vector<VectorPass>& passes,
                             const MapRenderSnapshot& snapshot,
                             const RenderRequest& request)
{
	for (auto color = snapshot.colors().rbegin(); color != snapshot.colors().rend(); ++color)
	{
		if (color->second.spot_method != SpotMethod::Spot)
			continue;
		passes.push_back({
			snapshot.buildColorSeparationIR(request, color->first, true),
			BlendMode::Multiply,
			1,
			true,
		});
	}

#if MAPPER_OVERPRINTING_CORRECTION > 0
	auto spot_request = request;
	spot_request.options |= RenderConfig::RequireSpotColor;
	passes.push_back({
		snapshot.buildIR(spot_request),
		BlendMode::SourceOver,
		overprintingCorrectionOpacity(),
		true,
	});
#endif

	if (request.options.testFlag(RenderConfig::Screen))
	{
		passes.push_back({
			snapshot.buildColorSeparationIR(
				request, MapColor::Reserved, true
			),
		});
	}
}

}  // namespace

FramePacketPtr FramePlanner::plan(const MapRenderSnapshot& snapshot, const FrameRequest& request)
{
	if (next_frame_id_ == std::numeric_limits<FrameId>::max())
		qFatal("Renderer frame id space exhausted");
	auto const& transform = request.view.world_to_viewport;
	if (!std::isfinite(request.view.device_pixel_ratio)
	    || request.view.device_pixel_ratio <= 0
	    || !std::isfinite(request.render.scaling)
	    || request.render.scaling <= 0
	    || !std::isfinite(request.render.opacity)
	    || request.render.opacity < 0
	    || request.render.opacity > 1
	    || !std::isfinite(transform.m11)
	    || !std::isfinite(transform.m12)
	    || !std::isfinite(transform.m21)
	    || !std::isfinite(transform.m22)
	    || !std::isfinite(transform.dx)
	    || !std::isfinite(transform.dy))
	{
		qFatal("Invalid renderer frame request");
	}

	auto frame = std::make_shared<FramePacket>();
	frame->id = next_frame_id_++;
	frame->revision = snapshot.revision();
	frame->view = request.view;
	frame->vector_passes = request.below_map;

	if (cached_snapshot_identity_ != snapshot.identity()
	    || cached_revision_ != snapshot.revision()
	    || cached_overprinting_ != request.simulate_overprinting
	    || !sameRequest(cached_request_, request.render))
	{
		planMapPasses(snapshot, request);
	}
	frame->vector_passes.insert(frame->vector_passes.end(),
	                            cached_map_passes_.begin(), cached_map_passes_.end());
	frame->vector_passes.insert(
		frame->vector_passes.end(), request.above_map.begin(), request.above_map.end()
	);

	return frame;
}

void FramePlanner::planMapPasses(const MapRenderSnapshot& snapshot, const FrameRequest& request)
{
	auto const chunkable = !request.simulate_overprinting && request.render.opacity == 1;
	if (!chunkable)
	{
		std::vector<VectorPass> map_passes;
		if (request.simulate_overprinting)
			appendOverprintingFrame(map_passes, snapshot, request.render);
		else
			appendNormalFrame(map_passes, snapshot, request.render);
		cached_chunks_.clear();
		cached_map_passes_ = std::move(map_passes);
	}
	else
	{
		// Chunks are keyed on the request, so any viewport or option change
		// re-records everything. A new revision re-records only the spans whose
		// membership, color, or object blocks changed; every other span keeps
		// its scene identity and thus its retained backend encoding.
		auto previous = std::move(cached_chunks_);
		if (!sameRequest(cached_request_, request.render))
			previous.clear();
		auto reusable = [&](const CachedChunk& chunk,
		                    const SnapshotObjectIds& ids, std::size_t begin, std::size_t end,
		                    int priority, const Color& draw_color) {
			return chunk.ids == ids
			       && chunk.begin == begin
			       && chunk.end == end
			       && chunk.priority == priority
			       && chunk.draw_color == draw_color
			       && spanBlocksUnchanged(
			              snapshot,
			              std::span(*ids).subspan(begin, end - begin),
			              chunk.recorded_revision);
		};

		std::vector<CachedChunk> chunks;
		chunks.reserve(previous.size() + 8);
		auto previous_next = previous.begin();
		auto const& color_objects = snapshot.colorObjects();
		for (auto bucket = color_objects.rbegin(); bucket != color_objects.rend(); ++bucket)
		{
			auto const priority = bucket->first;
			auto const& ids = bucket->second;
			if (!ids || ids->empty())
				continue;
			auto const draw_color = snapshot.drawColor(priority, request.render.options);
			if (!draw_color)
				continue;
			for (std::size_t begin = 0; begin < ids->size(); begin += frame_chunk_span_size)
			{
				auto const end = std::min(begin + frame_chunk_span_size, ids->size());
				// Unchanged snapshots yield chunks in the previous order, so the
				// last reused position finds the successor without searching.
				auto found = std::find_if(previous_next, previous.end(),
					[&](const CachedChunk& chunk) {
						return chunk.priority == priority && chunk.begin == begin;
					});
				if (found == previous.end() && previous_next != previous.begin())
				{
					found = std::find_if(previous.begin(), previous_next,
						[&](const CachedChunk& chunk) {
							return chunk.priority == priority && chunk.begin == begin;
						});
					if (found == previous_next)
						found = previous.end();
				}
				if (found != previous.end()
				    && reusable(*found, ids, begin, end, priority, *draw_color))
				{
					chunks.push_back(*found);
					previous_next = std::next(found);
					continue;
				}
				auto const reserve_hint = found != previous.end() && found->scene
				                        ? found->scene->commands.size()
				                        : std::size_t(0);
				chunks.push_back({
					snapshot.buildSpanIR(
						request.render, priority, *draw_color,
						std::span(*ids).subspan(begin, end - begin),
						reserve_hint),
					ids, begin, end, priority, *draw_color,
					snapshot.revision(),
				});
			}
		}

		std::vector<VectorPass> map_passes;
		map_passes.reserve(chunks.size());
		for (auto const& chunk : chunks)
		{
			if (chunk.scene && !chunk.scene->commands.empty())
				map_passes.push_back({ chunk.scene });
		}
		if (map_passes.empty())
		{
			// Consumers rely on every planned frame carrying a map pass even
			// when nothing is visible, so record the (empty) scene directly.
			map_passes.push_back({ snapshot.buildIR(request.render) });
		}
		cached_chunks_ = std::move(chunks);
		cached_map_passes_ = std::move(map_passes);
	}
	cached_snapshot_identity_ = snapshot.identity();
	cached_revision_ = snapshot.revision();
	cached_request_ = request.render;
	cached_overprinting_ = request.simulate_overprinting;
}

FramePacketPtr FramePlanner::plan(const FrameRequest& request)
{
	if (next_frame_id_ == std::numeric_limits<FrameId>::max())
		qFatal("Renderer frame id space exhausted");
	auto const& transform = request.view.world_to_viewport;
	if (!std::isfinite(request.view.device_pixel_ratio)
	    || request.view.device_pixel_ratio <= 0
	    || !std::isfinite(request.render.scaling)
	    || request.render.scaling <= 0
	    || !std::isfinite(request.render.opacity)
	    || request.render.opacity < 0
	    || request.render.opacity > 1
	    || !std::isfinite(transform.m11)
	    || !std::isfinite(transform.m12)
	    || !std::isfinite(transform.m21)
	    || !std::isfinite(transform.m22)
	    || !std::isfinite(transform.dx)
	    || !std::isfinite(transform.dy))
	{
		qFatal("Invalid renderer frame request");
	}

	auto frame = std::make_shared<FramePacket>();
	frame->id = next_frame_id_++;
	frame->view = request.view;
	frame->vector_passes = request.below_map;
	frame->vector_passes.insert(
		frame->vector_passes.end(), request.above_map.begin(), request.above_map.end()
	);
	for (auto const& pass : frame->vector_passes)
	{
		if (pass.scene)
			frame->revision = std::max(frame->revision, pass.scene->revision);
	}
	return frame;
}

FramePacketPtr FramePlanner::cameraFrame(FrameView view, Revision content_revision)
{
	if (next_frame_id_ == std::numeric_limits<FrameId>::max())
		qFatal("Renderer frame id space exhausted");
	auto const& transform = view.world_to_viewport;
	if (content_revision == 0
	    || !std::isfinite(view.device_pixel_ratio)
	    || view.device_pixel_ratio <= 0
	    || !std::isfinite(transform.m11)
	    || !std::isfinite(transform.m12)
	    || !std::isfinite(transform.m21)
	    || !std::isfinite(transform.m22)
	    || !std::isfinite(transform.dx)
	    || !std::isfinite(transform.dy))
	{
		qFatal("Invalid renderer camera request");
	}
	auto frame = std::make_shared<FramePacket>();
	frame->id = next_frame_id_++;
	frame->revision = content_revision;
	frame->view = view;
	return frame;
}

}  // namespace OpenOrienteering::render
