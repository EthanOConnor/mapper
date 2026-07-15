/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_VELLO_RENDERER_H
#define OPENORIENTEERING_VELLO_RENDERER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "presentation/native_surface_state.h"
#include "render/frame_packet.h"

namespace OpenOrienteering::render {

struct VelloFrameResult
{
	FrameCompletion completion;
	Revision revision = 0;
	std::uint64_t surface_sequence = 0;
	std::uint32_t scene_count = 0;
	std::uint64_t render_cpu_us = 0;
};

struct VelloImage
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<std::uint8_t> rgba8;
};

/**
 * Typed C++ front end to the Rust-owned Vello/wgpu render thread.
 *
 * RenderIR instances are encoded once and retained while their immutable C++
 * source remains alive. A submission clones only small Arc handles into the
 * bounded latest-wins frame channel.
 */
class VelloRenderer
{
public:
	VelloRenderer();
	~VelloRenderer();

	VelloRenderer(const VelloRenderer&) = delete;
	VelloRenderer& operator=(const VelloRenderer&) = delete;

	bool setSurface(const presentation::NativeSurfaceState& state);
	bool submit(const FramePacketPtr& frame,
	            const presentation::NativeSurfaceState& surface,
	            Color background = { 65535, 65535, 65535, 65535 });
	std::optional<VelloImage> renderOffscreen(
		const FramePacketPtr& frame,
		Color background = { 65535, 65535, 65535, 65535 }
	);
	std::optional<VelloFrameResult> takeResult();
	std::string lastError() const;
	std::size_t cachedSceneCount() const noexcept;
	std::size_t encodedSceneCount() const noexcept;
	std::size_t cachedImageCount() const noexcept;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace OpenOrienteering::render

#endif
