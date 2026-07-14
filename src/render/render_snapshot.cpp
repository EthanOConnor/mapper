/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/render_snapshot.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

#include <QColor>

#include "render/qt_render_bridge.h"

namespace OpenOrienteering::render {

namespace {

std::uint64_t nextSnapshotIdentity()
{
	static std::atomic<std::uint64_t> next { 1 };
	auto const identity = next.fetch_add(1, std::memory_order_relaxed);
	if (identity == std::numeric_limits<std::uint64_t>::max())
		qFatal("Render snapshot identity space exhausted");
	return identity;
}

Color highlighted(Color original)
{
	auto color = toQColor(original);
	if (color.value() > 127)
	{
		constexpr qreal factor = 0.35;
		color = QColor(factor * color.red(), factor * color.green(), factor * color.blue(), 255);
	}
	else
	{
		constexpr qreal factor = 0.15;
		color = QColor(
			255 - factor * (255 - color.red()),
			255 - factor * (255 - color.green()),
			255 - factor * (255 - color.blue()),
			255
		);
	}
	return fromQColor(color);
}

}  // namespace

MapRenderSnapshot::MapRenderSnapshot(Revision revision,
	                                 std::map<int, SnapshotColor> colors,
	                                 std::vector<SnapshotObjectBlockPtr> object_blocks,
	                                 std::map<int, SnapshotObjectIds> color_objects,
	                                 std::size_t object_count)
 : revision_(revision)
 , identity_(nextSnapshotIdentity())
 , colors_(std::move(colors))
 , object_blocks_(std::move(object_blocks))
 , color_objects_(std::move(color_objects))
 , object_count_(object_count)
{
	// All collections are immutable and structurally shared with adjacent revisions.
}

Revision MapRenderSnapshot::revision() const noexcept
{
	return revision_;
}

std::uint64_t MapRenderSnapshot::identity() const noexcept
{
	return identity_;
}

const std::map<int, SnapshotColor>& MapRenderSnapshot::colors() const noexcept
{
	return colors_;
}

std::size_t MapRenderSnapshot::objectCount() const noexcept
{
	return object_count_;
}

ObjectId MapRenderSnapshot::maxObjectId() const noexcept
{
	return ObjectId(object_blocks_.size()) * snapshot_object_block_size;
}

const SnapshotObject* MapRenderSnapshot::object(ObjectId id) const noexcept
{
	if (id == 0)
		return nullptr;
	auto const index = id - 1;
	auto const block_index = std::size_t(index / snapshot_object_block_size);
	if (block_index >= object_blocks_.size() || !object_blocks_[block_index])
		return nullptr;
	return object_blocks_[block_index]->objects[index % snapshot_object_block_size].get();
}

const SnapshotColor* MapRenderSnapshot::color(int priority) const
{
	auto const found = colors_.find(priority);
	return found == colors_.end() ? nullptr : &found->second;
}

bool MapRenderSnapshot::appendObjectColor(RenderIRBuilder& builder,
	                                      const SnapshotObject& object,
	                                      int color_priority,
	                                      Color draw_color,
	                                      const RenderRequest& request) const
{
	auto const found = object.colors.find(color_priority);
	if (found == object.colors.end() || !found->second)
		return false;

	auto emitted = false;
	PathPtr active_clip;
	auto const request_rect = toQRectF(request.bounding_box);
	for (auto const& item : *found->second)
	{
		if (!item.renderable)
			continue;
#ifdef Q_OS_ANDROID
		auto const& extent = item.renderable->getExtent();
		auto const min_dimension = 1.0 / request.scaling;
		if (extent.width() < min_dimension && extent.height() < min_dimension)
			continue;
#endif
		if (!item.renderable->intersects(request_rect))
			continue;

		if (active_clip != item.clip_path)
		{
			if (active_clip)
				builder.popClip();
			active_clip = item.clip_path;
			if (active_clip)
				builder.pushClip(active_clip);
		}

		auto const before = builder.commandCount();
		item.renderable->appendTo(builder, {
			request.scaling,
			request.options,
			draw_color,
			object.id,
		});
		emitted |= builder.commandCount() != before;
	}
	if (active_clip)
		builder.popClip();
	return emitted;
}

std::shared_ptr<const RenderIR> MapRenderSnapshot::buildIR(const RenderRequest& request) const
{
	RenderIRBuilder builder(revision_, request.bounding_box);
	if (request.opacity != 1)
		builder.pushLayer(request.opacity);

	for (auto color_bucket = color_objects_.rbegin(); color_bucket != color_objects_.rend(); ++color_bucket)
	{
		auto const priority = color_bucket->first;
		auto const* map_color = color(priority);
		if (!map_color || priority == MapColor::Reserved)
			continue;
		if (request.options.testFlag(RenderConfig::RequireSpotColor)
		    && (priority < 0 || map_color->spot_method == SpotMethod::Undefined))
		{
			continue;
		}

		auto draw_color = map_color->color;
		if (priority >= 0 && map_color->opacity < 1)
			draw_color = draw_color.withAlpha(map_color->opacity);
		if (request.options.testFlag(RenderConfig::Highlighted))
			draw_color = highlighted(draw_color);

		if (!color_bucket->second)
			continue;
		for (auto object_id : *color_bucket->second)
		{
			auto const* object_ptr = object(object_id);
			if (!object_ptr)
				continue;
			auto const& object = *object_ptr;
			if (!request.options.testFlag(RenderConfig::HelperSymbols) && object.helper_symbol)
				continue;
			if (object.hidden_symbol || !object.extent.intersects(request.bounding_box))
				continue;
			appendObjectColor(builder, object, priority, draw_color, request);
		}
	}

	if (request.opacity != 1)
		builder.popLayer();
	return builder.finish();
}

std::shared_ptr<const RenderIR> MapRenderSnapshot::buildColorSeparationIR(
	const RenderRequest& request,
	int separation_priority,
	bool use_color) const
{
	RenderIRBuilder builder(revision_, request.bounding_box);
	auto drawing_started = false;
	auto const* separation = color(separation_priority);
	if (!separation)
		return builder.finish();

	if (request.opacity != 1)
		builder.pushLayer(request.opacity);

	for (auto color_bucket = color_objects_.rbegin(); color_bucket != color_objects_.rend(); ++color_bucket)
	{
		auto const priority = color_bucket->first;
		auto const* source = color(priority);
		if (!source)
			continue;

		auto const* spot = source;
		auto factor = 1.0f;
		if (priority > MapColor::Reserved)
		{
			if (separation_priority == MapColor::Reserved)
				continue;

			switch (source->spot_method)
			{
			case SpotMethod::Undefined:
				continue;
			case SpotMethod::Spot:
				if (source->priority != separation_priority)
				{
					if (drawing_started && source->knockout)
						factor = 0;
					else
						continue;
				}
				break;
			case SpotMethod::Custom:
			{
				auto const component = std::find_if(
					source->components.begin(), source->components.end(),
					[separation_priority](auto const& value) {
						return value.priority == separation_priority;
					}
				);
				if (component != source->components.end())
				{
					spot = separation;
					factor = component->factor;
				}
				else if (drawing_started && source->knockout)
				{
					spot = separation;
					factor = 0;
				}
				else
				{
					continue;
				}
				break;
			}
			}
		}
		else if (separation_priority == MapColor::Reserved)
		{
			if (priority == MapColor::Registration || priority == MapColor::Reserved)
				continue;
		}
		else if (priority == MapColor::Registration)
		{
			spot = separation;
		}
		else
		{
			continue;
		}

		auto const drawing = factor >= 0.0005f;
		if (!drawing && !drawing_started)
			continue;

		QColor output_color;
		if (!drawing)
		{
			output_color = Qt::white;
		}
		else if (use_color)
		{
			output_color = toQColor(spot->color);
			float c, m, y, k;
			output_color.getCmykF(&c, &m, &y, &k);
			output_color.setCmykF(c * factor, m * factor, y * factor, k * factor, 1);
		}
		else
		{
			output_color.setCmykF(0, 0, 0, factor, 1);
		}

		if (!color_bucket->second)
			continue;
		for (auto object_id : *color_bucket->second)
		{
			auto const* object_ptr = object(object_id);
			if (!object_ptr)
				continue;
			auto const& object = *object_ptr;
			if (!request.options.testFlag(RenderConfig::HelperSymbols) && object.helper_symbol)
				continue;
			if (object.hidden_symbol || !object.extent.intersects(request.bounding_box))
				continue;
			auto const emitted = appendObjectColor(
				builder, object, priority, fromQColor(output_color), request
			);
			drawing_started |= drawing && emitted;
		}
	}

	if (request.opacity != 1)
		builder.popLayer();
	return builder.finish();
}

}  // namespace OpenOrienteering::render
