/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/vello_renderer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <mapper-vello-cxx/lib.h>

namespace OpenOrienteering::render {

namespace {

namespace ffi = vello_ffi;

ffi::Transform ffiTransform(Transform value)
{
	ffi::Transform out;
	out.m11 = value.m11;
	out.m12 = value.m12;
	out.m21 = value.m21;
	out.m22 = value.m22;
	out.dx = value.dx;
	out.dy = value.dy;
	return out;
}

ffi::Rect ffiRect(Rect value)
{
	ffi::Rect out;
	out.x = value.x;
	out.y = value.y;
	out.width = value.width;
	out.height = value.height;
	return out;
}

ffi::Color ffiColor(Color value)
{
	ffi::Color out;
	out.red = value.red;
	out.green = value.green;
	out.blue = value.blue;
	out.alpha = value.alpha;
	return out;
}

ffi::Stroke ffiStroke(StrokeStyle value)
{
	ffi::Stroke out;
	out.width = value.width;
	out.cap = std::uint8_t(value.cap);
	out.join = std::uint8_t(value.join);
	// RenderIR follows QPen's full-width miter units; Kurbo follows SVG/PDF units.
	out.miter_limit = value.join == LineJoin::Miter && value.miter_limit > 0
	                ? std::hypot(1.0, 2.0 * value.miter_limit)
	                : value.miter_limit;
	return out;
}

std::uint8_t ffiFillRule(FillRule value)
{
	return value == FillRule::OddEven ? 0 : 1;
}

std::uint8_t ffiBlend(BlendMode value)
{
	return value == BlendMode::Multiply ? 1 : 0;
}

template<typename T>
rust::Slice<const T> slice(const std::vector<T>& values)
{
	return { values.data(), values.size() };
}

class PathCache
{
public:
	const std::vector<ffi::PathElement>& get(const PathPtr& path)
	{
		if (!path)
			return empty_;
		auto const found = paths_.find(path.get());
		if (found != paths_.end())
			return found->second;

		std::vector<ffi::PathElement> elements;
		elements.reserve(path->elements().size());
		for (auto const& source : path->elements())
		{
			ffi::PathElement element;
			element.verb = std::uint8_t(source.verb);
			element.x1 = source.points[0].x;
			element.y1 = source.points[0].y;
			element.x2 = source.points[1].x;
			element.y2 = source.points[1].y;
			element.x3 = source.points[2].x;
			element.y3 = source.points[2].y;
			elements.push_back(element);
		}
		return paths_.emplace(path.get(), std::move(elements)).first->second;
	}

private:
	std::unordered_map<const Path*, std::vector<ffi::PathElement>> paths_;
	std::vector<ffi::PathElement> empty_;
};

ffi::SurfaceState ffiSurface(const presentation::NativeSurfaceState& state)
{
	ffi::SurfaceState out;
	out.sequence = state.sequence;
	out.phase = std::uint8_t(state.phase);
	out.platform = std::uint8_t(state.native.platform);
	out.window = state.native.window;
	out.display = state.native.display;
	out.width = state.physical_width;
	out.height = state.physical_height;
	return out;
}

FrameStatus frameStatus(std::uint8_t value)
{
	switch (value)
	{
	case 1: return FrameStatus::Presented;
	case 2: return FrameStatus::TargetUnavailable;
	case 3: return FrameStatus::DroppedStale;
	case 4: return FrameStatus::SurfaceLost;
	default: return FrameStatus::Failed;
	}
}

}  // namespace

class VelloRenderer::Impl
{
public:
	struct Retained
	{
		explicit Retained(rust::Box<ffi::RetainedScene> scene)
		 : scene(std::move(scene))
		{}

		rust::Box<ffi::RetainedScene> scene;
	};

	struct CacheEntry
	{
		std::weak_ptr<const RenderIR> source;
		std::shared_ptr<const Retained> retained;
	};

	struct RetainedImage
	{
		explicit RetainedImage(rust::Box<ffi::RetainedImage> image)
		 : image(std::move(image))
		{}

		rust::Box<ffi::RetainedImage> image;
	};

	struct ImageCacheEntry
	{
		std::weak_ptr<const ImageData> source;
		std::shared_ptr<const RetainedImage> retained;
	};

	Impl()
	 : renderer(ffi::new_renderer())
	{}

	std::shared_ptr<const RetainedImage> retainImage(
		const std::shared_ptr<const ImageData>& image)
	{
		for (auto entry = images.begin(); entry != images.end(); )
		{
			if (entry->second.source.expired())
				entry = images.erase(entry);
			else
				++entry;
		}
		if (auto const found = images.find(image.get()); found != images.end())
		{
			if (auto source = found->second.source.lock(); source == image)
				return found->second.retained;
			images.erase(found);
		}

		auto const bytes = image->bytes();
		auto retained = std::make_shared<RetainedImage>(ffi::new_retained_image(
			rust::Slice<const std::uint8_t> { bytes.data(), bytes.size() },
			image->width, image->height, image->bytes_per_row
		));
		images.emplace(image.get(), ImageCacheEntry { image, retained });
		return retained;
	}

	std::shared_ptr<const Retained> encode(const std::shared_ptr<const RenderIR>& ir)
	{
		for (auto entry = scenes.begin(); entry != scenes.end(); )
		{
			if (entry->second.source.expired())
				entry = scenes.erase(entry);
			else
				++entry;
		}
		if (auto const found = scenes.find(ir.get()); found != scenes.end())
		{
			if (auto source = found->second.source.lock(); source == ir)
				return found->second.retained;
			scenes.erase(found);
		}

		auto builder = ffi::begin_scene(ir->revision, ffiRect(ir->world_bounds));
		PathCache paths;
		for (auto const& command : ir->commands)
		{
			std::visit([&](auto const& op) {
				using T = std::decay_t<decltype(op)>;
				if constexpr (std::is_same_v<T, PushTransform>)
				{
					ffi::scene_push_transform(*builder, ffiTransform(op.transform));
				}
				else if constexpr (std::is_same_v<T, PopTransform>)
				{
					ffi::scene_pop_transform(*builder);
				}
				else if constexpr (std::is_same_v<T, PushClip>)
				{
					auto const& elements = paths.get(op.path);
					ffi::scene_push_clip(*builder,
					                     op.path ? ffiFillRule(op.path->fillRule()) : 0,
					                     slice(elements));
				}
				else if constexpr (std::is_same_v<T, PopClip>)
				{
					ffi::scene_pop_clip(*builder);
				}
				else if constexpr (std::is_same_v<T, PushLayer>)
				{
					ffi::scene_push_layer(*builder, op.opacity, ffiBlend(op.blend));
				}
				else if constexpr (std::is_same_v<T, PopLayer>)
				{
					ffi::scene_pop_layer(*builder);
				}
				else if constexpr (std::is_same_v<T, FillPath>)
				{
					auto const& elements = paths.get(op.path);
					ffi::scene_fill_path(*builder,
					                    op.path ? ffiFillRule(op.path->fillRule()) : 0,
					                    slice(elements), ffiColor(op.color));
				}
				else if constexpr (std::is_same_v<T, StrokePath>)
				{
					auto const& elements = paths.get(op.path);
					ffi::scene_stroke_path(
						*builder, slice(elements), ffiColor(op.color), ffiStroke(op.style),
						slice(op.style.dash_pattern), op.style.dash_offset
					);
				}
				else if constexpr (std::is_same_v<T, FillEllipse>)
				{
					ffi::scene_fill_ellipse(*builder, ffiRect(op.bounds), ffiColor(op.color));
				}
				else if constexpr (std::is_same_v<T, StrokeEllipse>)
				{
					ffi::scene_stroke_ellipse(
						*builder, ffiRect(op.bounds), ffiColor(op.color), ffiStroke(op.style)
					);
				}
				else if constexpr (std::is_same_v<T, DrawImage>)
				{
					if (!op.image || op.image->bytes().empty())
						throw std::logic_error("Vello received invalid immutable image data");
					auto const image = retainImage(op.image);
					auto const accepted = ffi::scene_draw_image(
						*builder, *image->image, ffiRect(op.source),
						ffiTransform(op.image_to_scene), op.opacity
					);
					if (!accepted)
						throw std::logic_error("Vello rejected immutable image data");
				}
				else if constexpr (std::is_same_v<T, DrawLinePattern>)
				{
					auto const& elements = paths.get(op.outline);
					ffi::LinePatternStyle style;
					style.color = ffiColor(op.color);
					style.angle = op.angle;
					style.spacing = op.spacing;
					style.offset = op.offset;
					style.line_width = op.line_width;
					ffi::scene_draw_line_pattern(
						*builder,
						op.outline ? ffiFillRule(op.outline->fillRule()) : 0,
						slice(elements), style
					);
				}
			}, command);
		}

		auto retained = std::make_shared<Retained>(ffi::finish_scene(std::move(builder)));
		if (ffi::retained_scene_revision(*retained->scene) != ir->revision
		    || ffi::retained_scene_command_count(*retained->scene) != ir->commands.size())
		{
			throw std::logic_error("Vello scene encoding violated the immutable IR contract");
		}
		scenes.emplace(ir.get(), CacheEntry { ir, retained });
		++encoded_scene_count;
		return retained;
	}

	rust::Box<ffi::FrameBuilder> buildFrame(const FramePacket& frame,
	                                      std::uint32_t width,
	                                      std::uint32_t height,
	                                      double device_pixel_ratio,
	                                      std::uint64_t surface_sequence,
	                                      Color background)
	{
		auto world_transform = frame.view.world_to_viewport;
		world_transform.m11 *= device_pixel_ratio;
		world_transform.m12 *= device_pixel_ratio;
		world_transform.m21 *= device_pixel_ratio;
		world_transform.m22 *= device_pixel_ratio;
		world_transform.dx *= device_pixel_ratio;
		world_transform.dy *= device_pixel_ratio;
		Transform const viewport_transform {
			device_pixel_ratio, 0, 0, device_pixel_ratio, 0, 0
		};

		ffi::FrameHeader header;
		header.frame_id = frame.id;
		header.revision = frame.revision;
		header.surface_sequence = surface_sequence;
		header.width = width;
		header.height = height;
		header.world_to_surface = ffiTransform(world_transform);
		header.background = ffiColor(background);
		auto request = ffi::new_frame(header);
		for (auto const& pass : frame.vector_passes)
		{
			if (!pass.scene)
				continue;
			auto const retained = encode(pass.scene);
			ffi::frame_add_pass(
				*request, *retained->scene, ffiBlend(pass.blend), pass.opacity, pass.isolated,
				ffiTransform(pass.space == VectorPass::Space::World
				             ? world_transform : viewport_transform)
			);
		}
		return request;
	}

	rust::Box<ffi::Renderer> renderer;
	std::unordered_map<const RenderIR*, CacheEntry> scenes;
	std::unordered_map<const ImageData*, ImageCacheEntry> images;
	std::size_t encoded_scene_count = 0;
};

VelloRenderer::VelloRenderer()
 : impl_(std::make_unique<Impl>())
{}

VelloRenderer::~VelloRenderer() = default;

bool VelloRenderer::setSurface(const presentation::NativeSurfaceState& state)
{
	return ffi::renderer_set_surface(*impl_->renderer, ffiSurface(state));
}

bool VelloRenderer::submit(const FramePacketPtr& frame,
	                       const presentation::NativeSurfaceState& surface,
	                       Color background)
{
	if (!frame || frame->id == 0 || frame->revision == 0
	    || !std::isfinite(surface.device_pixel_ratio)
	    || surface.device_pixel_ratio <= 0
	    || surface.phase != presentation::SurfacePhase::Exposed
	    || surface.sequence == 0 || surface.physical_width == 0
	    || surface.physical_height == 0)
	{
		return false;
	}
	auto request = impl_->buildFrame(
		*frame, surface.physical_width, surface.physical_height,
		surface.device_pixel_ratio, surface.sequence, background
	);
	return ffi::renderer_submit(*impl_->renderer, std::move(request));
}

std::optional<VelloImage> VelloRenderer::renderOffscreen(
	const FramePacketPtr& frame, Color background)
{
	if (!frame || frame->id == 0 || frame->revision == 0
	    || frame->view.width == 0 || frame->view.height == 0
	    || !std::isfinite(frame->view.device_pixel_ratio)
	    || frame->view.device_pixel_ratio <= 0)
	{
		return std::nullopt;
	}
	auto const physical_width = std::ceil(
		double(frame->view.width) * frame->view.device_pixel_ratio
	);
	auto const physical_height = std::ceil(
		double(frame->view.height) * frame->view.device_pixel_ratio
	);
	if (physical_width > std::numeric_limits<std::uint32_t>::max()
	    || physical_height > std::numeric_limits<std::uint32_t>::max())
	{
		return std::nullopt;
	}
	auto const width = std::uint32_t(physical_width);
	auto const height = std::uint32_t(physical_height);
	auto request = impl_->buildFrame(
		*frame, width, height, frame->view.device_pixel_ratio, 0, background
	);
	auto pixels = ffi::renderer_render_offscreen(*impl_->renderer, std::move(request));
	auto const expected_size = std::size_t(width) * std::size_t(height) * 4;
	if (pixels.size() != expected_size)
		return std::nullopt;
	return VelloImage {
		width,
		height,
		{ pixels.begin(), pixels.end() },
	};
}

std::optional<VelloFrameResult> VelloRenderer::takeResult()
{
	auto const result = ffi::renderer_try_take_result(*impl_->renderer);
	if (result.status == 0)
		return std::nullopt;
	return VelloFrameResult {
		{ result.frame_id, frameStatus(result.status) },
		result.revision,
		result.surface_sequence,
		result.scene_count,
	};
}

std::string VelloRenderer::lastError() const
{
	auto const error = ffi::renderer_last_error(*impl_->renderer);
	return { error.data(), error.size() };
}

std::size_t VelloRenderer::cachedSceneCount() const noexcept
{
	return impl_->scenes.size();
}

std::size_t VelloRenderer::encodedSceneCount() const noexcept
{
	return impl_->encoded_scene_count;
}

std::size_t VelloRenderer::cachedImageCount() const noexcept
{
	return impl_->images.size();
}

}  // namespace OpenOrienteering::render
