/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/vello_renderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
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

	struct PreparedPass
	{
		std::shared_ptr<const Retained> scene;
		BlendMode blend = BlendMode::SourceOver;
		double opacity = 1;
		bool isolated = false;
		VectorPass::Space space = VectorPass::Space::World;
	};

	struct PreparedGeneration
	{
		Revision revision = 0;
		/** Keeps immutable IR, image storage, and raster accounting leases alive. */
		FramePacketPtr source;
		std::vector<PreparedPass> passes;
	};

	struct ViewSubmission
	{
		FrameId frame_id = 0;
		FrameView view;
		presentation::NativeSurfaceState surface;
		Color background;
	};

	struct EncodeRequest
	{
		FramePacketPtr frame;
		ViewSubmission view;
		std::uint64_t serial = 0;
	};

	struct PresentationRequest
	{
		ViewSubmission view;
		std::shared_ptr<const PreparedGeneration> generation;
	};

	Impl()
	 : renderer(ffi::new_renderer())
	 , encoder_thread([this] { encodeLoop(); })
	 , presenter_thread([this] { presentLoop(); })
	{}

	~Impl()
	{
		{
			std::lock_guard lock(state_mutex);
			stopping = true;
			pending_encode.reset();
			pending_presentation.reset();
		}
		state_changed.notify_all();
		if (encoder_thread.joinable())
			encoder_thread.join();
		if (presenter_thread.joinable())
			presenter_thread.join();
	}

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
			image->width, image->height, image->bytes_per_row,
			image->alpha_type == ImageAlphaType::Premultiplied ? 1 : 0
		));
		images.emplace(image.get(), ImageCacheEntry { image, retained });
		cached_image_count.store(images.size(), std::memory_order_relaxed);
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
		cached_scene_count.store(scenes.size(), std::memory_order_relaxed);
		encoded_scene_count.fetch_add(1, std::memory_order_relaxed);
		return retained;
	}

	std::shared_ptr<const PreparedGeneration> encodeGeneration(const FramePacketPtr& frame)
	{
		auto generation = std::make_shared<PreparedGeneration>();
		generation->revision = frame->revision;
		generation->source = frame;
		generation->passes.reserve(frame->vector_passes.size());
		for (auto const& pass : frame->vector_passes)
		{
			if (!pass.scene)
				continue;
			generation->passes.push_back({
				encode(pass.scene), pass.blend, pass.opacity, pass.isolated, pass.space,
			});
		}
		cached_scene_count.store(scenes.size(), std::memory_order_relaxed);
		cached_image_count.store(images.size(), std::memory_order_relaxed);
		return generation;
	}

	rust::Box<ffi::FrameBuilder> buildFrame(
		const PreparedGeneration& generation,
		const ViewSubmission& submission)
	{
		auto world_transform = submission.view.world_to_viewport;
		auto const device_pixel_ratio = submission.surface.device_pixel_ratio;
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
		header.frame_id = submission.frame_id;
		header.revision = generation.revision;
		header.surface_sequence = submission.surface.sequence;
		header.width = submission.surface.physical_width;
		header.height = submission.surface.physical_height;
		header.world_to_surface = ffiTransform(world_transform);
		header.background = ffiColor(submission.background);
		auto request = ffi::new_frame(header);
		for (auto const& pass : generation.passes)
		{
			ffi::frame_add_pass(
				*request, *pass.scene->scene, ffiBlend(pass.blend), pass.opacity, pass.isolated,
				ffiTransform(pass.space == VectorPass::Space::World
				             ? world_transform : viewport_transform)
			);
		}
		return request;
	}

	bool queueContent(
		const FramePacketPtr& frame,
		const presentation::NativeSurfaceState& surface,
		Color background)
	{
		std::lock_guard lock(state_mutex);
		if (stopping || latest_content_serial == std::numeric_limits<std::uint64_t>::max())
			return false;
		ViewSubmission view { frame->id, frame->view, surface, background };
		latest_view = view;
		content_pending.store(true, std::memory_order_release);
		pending_encode = EncodeRequest { frame, view, ++latest_content_serial };
		state_changed.notify_all();
		return true;
	}

	bool queueCamera(
		FrameId frame_id,
		const FrameView& view,
		const presentation::NativeSurfaceState& surface,
		Color background)
	{
		std::lock_guard lock(state_mutex);
		if (stopping)
			return false;
		latest_view = ViewSubmission { frame_id, view, surface, background };
		if (auto generation = completed_generation)
			pending_presentation = PresentationRequest { *latest_view, std::move(generation) };
		state_changed.notify_all();
		return true;
	}

	void encodeLoop()
	{
		for (;;)
		{
			EncodeRequest request;
			{
				std::unique_lock lock(state_mutex);
				state_changed.wait(lock, [this] { return stopping || pending_encode.has_value(); });
				if (stopping)
					return;
				request = std::move(*pending_encode);
				pending_encode.reset();
			}

			try
			{
				std::shared_ptr<const PreparedGeneration> generation;
				{
					std::lock_guard lock(encode_mutex);
					generation = encodeGeneration(request.frame);
				}
				std::lock_guard lock(state_mutex);
				if (stopping)
					return;
				// Never flash an obsolete partially staged generation. The newer
				// request remains queued and benefits from the warmed retained cache.
				if (request.serial != latest_content_serial)
					continue;
				{
					std::lock_guard error_lock(error_mutex);
					encoder_error.clear();
				}
				completed_generation = generation;
				content_pending.store(false, std::memory_order_release);
				auto const view = latest_view.value_or(request.view);
				pending_presentation = PresentationRequest { view, std::move(generation) };
				state_changed.notify_all();
			}
			catch (const std::exception& error)
			{
				if (markFailedIfCurrent(request.serial))
					pushFailure(request.view, error.what());
			}
			catch (...)
			{
				if (markFailedIfCurrent(request.serial))
					pushFailure(request.view, "Vello content encoding failed");
			}
		}
	}

	void presentLoop()
	{
		for (;;)
		{
			PresentationRequest request;
			{
				std::unique_lock lock(state_mutex);
				state_changed.wait(lock, [this] {
					return stopping || pending_presentation.has_value();
				});
				if (stopping)
					return;
				request = std::move(*pending_presentation);
				pending_presentation.reset();
			}
			try
			{
				auto frame = buildFrame(*request.generation, request.view);
				if (!ffi::renderer_submit(*renderer, std::move(frame)))
					pushFailure(request.view, "Vello render thread rejected a prepared frame");
			}
			catch (const std::exception& error)
			{
				pushFailure(request.view, error.what());
			}
			catch (...)
			{
				pushFailure(request.view, "Vello frame presentation failed");
			}
		}
	}

	void pushFailure(const ViewSubmission& view, std::string message)
	{
		{
			std::lock_guard lock(error_mutex);
			encoder_error = std::move(message);
		}
		std::lock_guard lock(failure_mutex);
		if (failures.size() == 64)
			failures.pop_front();
		failures.push_back({
			{ view.frame_id, FrameStatus::Failed },
			0,
			view.surface.sequence,
			0,
		});
	}

	bool markFailedIfCurrent(std::uint64_t serial)
	{
		std::lock_guard lock(state_mutex);
		if (serial != latest_content_serial)
			return false;
		content_pending.store(false, std::memory_order_release);
		return true;
	}

	std::optional<VelloFrameResult> takeFailure()
	{
		std::lock_guard lock(failure_mutex);
		if (failures.empty())
			return {};
		auto result = failures.front();
		failures.pop_front();
		return result;
	}

	rust::Box<ffi::Renderer> renderer;
	std::mutex encode_mutex;
	std::unordered_map<const RenderIR*, CacheEntry> scenes;
	std::unordered_map<const ImageData*, ImageCacheEntry> images;
	std::atomic_size_t cached_scene_count { 0 };
	std::atomic_size_t cached_image_count { 0 };
	std::atomic_size_t encoded_scene_count { 0 };
	std::atomic_bool content_pending { false };
	std::shared_ptr<const PreparedGeneration> completed_generation;

	std::mutex state_mutex;
	std::condition_variable state_changed;
	std::optional<EncodeRequest> pending_encode;
	std::optional<PresentationRequest> pending_presentation;
	std::optional<ViewSubmission> latest_view;
	std::uint64_t latest_content_serial = 0;
	bool stopping = false;
	std::thread encoder_thread;
	std::thread presenter_thread;

	std::mutex failure_mutex;
	std::deque<VelloFrameResult> failures;
	mutable std::mutex error_mutex;
	std::string encoder_error;
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
	return impl_->queueContent(frame, surface, background);
}

bool VelloRenderer::submitCamera(
	FrameId frame_id,
	const FrameView& view,
	const presentation::NativeSurfaceState& surface,
	Color background)
{
	auto const& transform = view.world_to_viewport;
	if (frame_id == 0
	    || !std::isfinite(view.device_pixel_ratio)
	    || view.device_pixel_ratio <= 0
	    || !std::isfinite(transform.m11) || !std::isfinite(transform.m12)
	    || !std::isfinite(transform.m21) || !std::isfinite(transform.m22)
	    || !std::isfinite(transform.dx) || !std::isfinite(transform.dy)
	    || !std::isfinite(surface.device_pixel_ratio)
	    || surface.device_pixel_ratio <= 0
	    || surface.phase != presentation::SurfacePhase::Exposed
	    || surface.sequence == 0 || surface.physical_width == 0
	    || surface.physical_height == 0)
	{
		return false;
	}
	return impl_->queueCamera(frame_id, view, surface, background);
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
	presentation::NativeSurfaceState surface;
	surface.phase = presentation::SurfacePhase::Exposed;
	surface.physical_width = width;
	surface.physical_height = height;
	surface.device_pixel_ratio = frame->view.device_pixel_ratio;
	Impl::ViewSubmission view { frame->id, frame->view, surface, background };
	rust::Box<ffi::FrameBuilder> request = [&] {
		std::lock_guard lock(impl_->encode_mutex);
		auto generation = impl_->encodeGeneration(frame);
		return impl_->buildFrame(*generation, view);
	}();
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
	if (auto failure = impl_->takeFailure())
		return failure;
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
	{
		std::lock_guard lock(impl_->error_mutex);
		if (!impl_->encoder_error.empty())
			return impl_->encoder_error;
	}
	auto const error = ffi::renderer_last_error(*impl_->renderer);
	return { error.data(), error.size() };
}

std::size_t VelloRenderer::cachedSceneCount() const noexcept
{
	return impl_->cached_scene_count.load(std::memory_order_relaxed);
}

std::size_t VelloRenderer::encodedSceneCount() const noexcept
{
	return impl_->encoded_scene_count.load(std::memory_order_relaxed);
}

std::size_t VelloRenderer::cachedImageCount() const noexcept
{
	return impl_->cached_image_count.load(std::memory_order_relaxed);
}

bool VelloRenderer::contentPending() const noexcept
{
	return impl_->content_pending.load(std::memory_order_acquire);
}

}  // namespace OpenOrienteering::render
