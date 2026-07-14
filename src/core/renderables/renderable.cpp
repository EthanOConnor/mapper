/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2012-2018 Kai Pastor
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    any later version.
 */

#include "core/renderables/renderable.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <QColor>

#include "core/map.h"
#include "core/objects/object.h"
#include "core/symbols/symbol.h"
#include "render/overlay_scene.h"
#include "render/qt_render_bridge.h"
#include "render/render_snapshot.h"
#include "util/util.h"

namespace OpenOrienteering {

namespace {

render::SpotMethod spotMethod(MapColor::ColorMethod method)
{
	switch (method)
	{
	case MapColor::CustomColor: return render::SpotMethod::Custom;
	case MapColor::SpotColor: return render::SpotMethod::Spot;
	default: return render::SpotMethod::Undefined;
	}
}

render::SnapshotColor snapshotColor(const MapColor& color)
{
	render::SnapshotColor snapshot;
	snapshot.priority = color.getPriority();
	snapshot.color = render::fromQColor(static_cast<const QColor&>(color));
	snapshot.opacity = color.getOpacity();
	snapshot.spot_method = spotMethod(color.getSpotColorMethod());
	snapshot.knockout = color.getKnockout();
	snapshot.components.reserve(color.getComponents().size());
	for (auto const& component : color.getComponents())
	{
		if (component.spot_color)
			snapshot.components.push_back({ component.spot_color->getPriority(), component.factor });
	}
	return snapshot;
}

}  // namespace

Renderable::~Renderable() = default;

ObjectRenderables::ObjectRenderables(Object& object)
 : extent(object.extent)
{
	// nothing
}

ObjectRenderables::~ObjectRenderables() = default;

std::shared_ptr<RenderableVector>& ObjectRenderables::writableColor(int color_priority)
{
	auto& renderables = operator[](color_priority);
	if (!renderables)
		renderables = std::make_shared<RenderableVector>();
	else if (renderables.use_count() != 1)
		renderables = std::make_shared<RenderableVector>(*renderables);
	return renderables;
}

void ObjectRenderables::insertRenderable(Renderable* renderable)
{
	std::shared_ptr<const Renderable> owned(renderable);
	writableColor(renderable->colorPriority())->push_back({ std::move(owned), clip_path });
	if (!clip_path)
	{
		if (extent.isValid())
			rectInclude(extent, renderable->getExtent());
		else
			extent = renderable->getExtent();
	}
}

void ObjectRenderables::clear()
{
	std::map<int, std::shared_ptr<RenderableVector>>::clear();
}

void ObjectRenderables::detach()
{
	for (auto& [priority, renderables] : *this)
	{
		Q_UNUSED(priority)
		if (renderables && renderables.use_count() != 1)
			renderables = std::make_shared<RenderableVector>(*renderables);
	}
}

std::shared_ptr<const render::RenderIR> ObjectRenderables::buildIR(
	int map_color, render::Color color, const render::RenderRequest& request) const
{
	render::RenderIRBuilder builder(0, request.bounding_box);
	auto const bounding_box = render::toQRectF(request.bounding_box);
	if (!extent.intersects(bounding_box))
		return builder.finish();
	auto const found = find(map_color);
	if (found == end() || !found->second)
		return builder.finish();

	if (request.opacity != 1)
		builder.pushLayer(request.opacity);
	render::PathPtr active_clip;
	for (auto const& item : *found->second)
	{
		if (!item.renderable || !item.renderable->intersects(bounding_box))
			continue;
		if (active_clip != item.clip_path)
		{
			if (active_clip)
				builder.popClip();
			active_clip = item.clip_path;
			if (active_clip)
				builder.pushClip(active_clip);
		}
		item.renderable->appendTo(builder, {
			request.scaling,
			request.options,
			color,
			0,
		});
	}
	if (active_clip)
		builder.popClip();
	if (request.opacity != 1)
		builder.popLayer();
	return builder.finish();
}

void ObjectRenderables::setClipPath(render::PathPtr path)
{
	clip_path = std::move(path);
}

void MapRenderables::ObjectDeleter::operator()(Object* object) const
{
	renderables.removeRenderablesOfObject(object, false);
	delete object;
}

MapRenderables::MapRenderables(Map* map)
 : map(map)
{
	// nothing
}

void MapRenderables::changed()
{
	if (Q_UNLIKELY(current_revision == std::numeric_limits<render::Revision>::max()))
		qFatal("Render revision exhausted");
	++current_revision;
	published_snapshot.reset();
}

void MapRenderables::markObjectBlockDirty(render::ObjectId id)
{
	Q_ASSERT(id > 0);
	dirty_blocks.insert(std::size_t((id - 1) / render::snapshot_object_block_size));
}

void MapRenderables::addToColorOrder(const ObjectEntry& entry)
{
	for (auto const& [priority, renderables] : entry.colors)
	{
		Q_UNUSED(renderables)
		if (color_object_ids[priority].insert(entry.id).second)
			dirty_color_priorities.insert(priority);
	}
}

void MapRenderables::removeFromColorOrder(const ObjectEntry& entry)
{
	for (auto const& [priority, renderables] : entry.colors)
	{
		Q_UNUSED(renderables)
		auto found = color_object_ids.find(priority);
		if (found == color_object_ids.end() || found->second.erase(entry.id) == 0)
			continue;
		dirty_color_priorities.insert(priority);
		if (found->second.empty())
			color_object_ids.erase(found);
	}
}

void MapRenderables::invalidate()
{
	for (auto& [object, entry] : objects)
	{
		Q_UNUSED(object)
		entry.snapshot.reset();
		markObjectBlockDirty(entry.id);
	}
	changed();
}

void MapRenderables::insertRenderablesOfObject(const Object* object)
{
	auto found = objects.find(object);
	auto const is_new = found == objects.end();
	std::set<int> previous_priorities;
	if (found == objects.end())
	{
		if (Q_UNLIKELY(next_object_id == std::numeric_limits<render::ObjectId>::max()))
			qFatal("Render object id exhausted");
		ObjectEntry entry;
		entry.id = next_object_id++;
		found = objects.emplace(object, std::move(entry)).first;
		object_order.emplace(found->second.id, object);
	}
	else
	{
		for (auto const& [priority, renderables] : found->second.colors)
		{
			Q_UNUSED(renderables)
			previous_priorities.insert(priority);
		}
		found->second.colors.clear();
		found->second.snapshot.reset();
	}

	for (auto const& [priority, renderables] : object->renderables())
	{
		if (renderables && !renderables->empty())
			found->second.colors.emplace(priority, renderables);
	}
	if (is_new)
	{
		addToColorOrder(found->second);
	}
	else
	{
		std::set<int> current_priorities;
		for (auto const& [priority, renderables] : found->second.colors)
		{
			Q_UNUSED(renderables)
			current_priorities.insert(priority);
		}
		for (auto priority : previous_priorities)
		{
			if (current_priorities.contains(priority))
				continue;
			auto indexed = color_object_ids.find(priority);
			Q_ASSERT(indexed != color_object_ids.end());
			indexed->second.erase(found->second.id);
			dirty_color_priorities.insert(priority);
			if (indexed->second.empty())
				color_object_ids.erase(indexed);
		}
		for (auto priority : current_priorities)
		{
			if (previous_priorities.contains(priority))
				continue;
			color_object_ids[priority].insert(found->second.id);
			dirty_color_priorities.insert(priority);
		}
	}
	markObjectBlockDirty(found->second.id);
	changed();
}

void MapRenderables::removeRenderablesOfObject(const Object* object, bool mark_area_as_dirty)
{
	auto const found = objects.find(object);
	if (found == objects.end())
		return;

	if (mark_area_as_dirty)
	{
		auto dirty = object->getExtent();
		if (!dirty.isValid())
		{
			for (auto const& [priority, renderables] : found->second.colors)
			{
				Q_UNUSED(priority)
				if (!renderables)
					continue;
				for (auto const& item : *renderables)
				{
					if (!item.renderable)
						continue;
					dirty = dirty.isValid() ? dirty.united(item.renderable->getExtent())
					                        : item.renderable->getExtent();
				}
			}
		}
		map->setObjectAreaDirty(dirty);
	}

	removeFromColorOrder(found->second);
	object_order.erase(found->second.id);
	markObjectBlockDirty(found->second.id);
	objects.erase(found);
	changed();
}

void MapRenderables::clear(bool mark_area_as_dirty)
{
	if (mark_area_as_dirty)
	{
		for (auto const& [object, entry] : objects)
		{
			Q_UNUSED(entry)
			map->setObjectAreaDirty(object->getExtent());
		}
	}
	objects.clear();
	object_order.clear();
	color_object_ids.clear();
	next_object_id = 1;
	snapshot_blocks.clear();
	dirty_blocks.clear();
	snapshot_color_objects.clear();
	dirty_color_priorities.clear();
	changed();
}

std::shared_ptr<const render::MapRenderSnapshot> MapRenderables::snapshot() const
{
	if (published_snapshot)
		return published_snapshot;

	std::map<int, render::SnapshotColor> colors;
	for (int i = 0; i < map->getNumColors(); ++i)
	{
		if (auto const* color = map->getColor(i))
			colors.emplace(i, snapshotColor(*color));
	}
	for (auto const& [priority, ids] : color_object_ids)
	{
		Q_UNUSED(ids)
		if (!colors.contains(priority))
		{
			if (auto const* color = map->getColor(priority))
				colors.emplace(priority, snapshotColor(*color));
		}
	}
	// The undefined separation is requested explicitly by overprint screen output.
	if (!colors.contains(MapColor::Reserved))
		colors.emplace(MapColor::Reserved, snapshotColor(*Map::getUndefinedColor()));

	for (auto block_index : dirty_blocks)
	{
		if (snapshot_blocks.size() <= block_index)
			snapshot_blocks.resize(block_index + 1);
		auto block = std::make_shared<render::SnapshotObjectBlock>();
		auto const first_id = render::ObjectId(block_index * render::snapshot_object_block_size + 1);
		auto const end_id = first_id + render::snapshot_object_block_size;
		for (auto ordered = object_order.lower_bound(first_id);
		     ordered != object_order.end() && ordered->first < end_id;
		     ++ordered)
		{
			auto const id = ordered->first;
			auto const* object = ordered->second;
			auto const found = objects.find(object);
			if (found == objects.end())
				continue;
			if (!found->second.snapshot)
			{
				auto const* symbol = object->getSymbol();
				found->second.snapshot = std::make_shared<const render::SnapshotObject>(
					render::SnapshotObject {
						id,
						render::fromQRectF(object->getExtent()),
						symbol && symbol->isHelperSymbol(),
						symbol && symbol->isHidden(),
						found->second.colors,
					}
				);
			}
			block->objects[(id - 1) % render::snapshot_object_block_size] = found->second.snapshot;
		}
		snapshot_blocks[block_index] = std::move(block);
	}
	dirty_blocks.clear();

	for (auto priority : dirty_color_priorities)
	{
		auto const found = color_object_ids.find(priority);
		if (found == color_object_ids.end())
		{
			snapshot_color_objects.erase(priority);
			continue;
		}
		snapshot_color_objects[priority] =
			std::make_shared<const std::vector<render::ObjectId>>(
				found->second.begin(), found->second.end()
			);
	}
	dirty_color_priorities.clear();

	published_snapshot = std::make_shared<const render::MapRenderSnapshot>(
		current_revision,
		std::move(colors),
		snapshot_blocks,
		snapshot_color_objects,
		objects.size()
	);
	return published_snapshot;
}

void MapRenderables::draw(render::OverlaySceneBuilder* painter,
	                      const RenderConfig& config) const
{
	if (!painter)
		return;
	painter->append(*snapshot()->buildIR({
		render::fromQRectF(config.bounding_box),
		config.scaling,
		config.options,
		config.opacity,
	}));
}

}  // namespace OpenOrienteering
