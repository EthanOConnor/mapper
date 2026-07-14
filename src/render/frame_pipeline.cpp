/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/frame_pipeline.h"

#include <cmath>
#include <limits>

#include <QtGlobal>

namespace OpenOrienteering::render {

namespace {

#ifndef MAPPER_OVERPRINTING_CORRECTION
#define MAPPER_OVERPRINTING_CORRECTION 2
#endif

void appendNormalFrame(FramePacket& frame, const MapRenderSnapshot& snapshot)
{
	frame.vector_passes.push_back({ snapshot.buildIR(frame.render_request) });
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

void appendOverprintingFrame(FramePacket& frame, const MapRenderSnapshot& snapshot)
{
	for (auto color = snapshot.colors().rbegin(); color != snapshot.colors().rend(); ++color)
	{
		if (color->second.spot_method != SpotMethod::Spot)
			continue;
		frame.vector_passes.push_back({
			snapshot.buildColorSeparationIR(frame.render_request, color->first, true),
			BlendMode::Multiply,
			1,
			true,
		});
	}

#if MAPPER_OVERPRINTING_CORRECTION > 0
	auto spot_request = frame.render_request;
	spot_request.options |= RenderConfig::RequireSpotColor;
	frame.vector_passes.push_back({
		snapshot.buildIR(spot_request),
		BlendMode::SourceOver,
		overprintingCorrectionOpacity(),
		true,
	});
#endif

	if (frame.render_request.options.testFlag(RenderConfig::Screen))
	{
		frame.vector_passes.push_back({
			snapshot.buildColorSeparationIR(
				frame.render_request, MapColor::Reserved, true
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
	frame->render_request = request.render;

	if (request.simulate_overprinting)
		appendOverprintingFrame(*frame, snapshot);
	else
		appendNormalFrame(*frame, snapshot);

	return frame;
}

}  // namespace OpenOrienteering::render
