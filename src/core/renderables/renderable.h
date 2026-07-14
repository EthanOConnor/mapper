/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2012-2017 Kai Pastor
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    any later version.
 */

#ifndef OPENORIENTEERING_RENDERABLE_H
#define OPENORIENTEERING_RENDERABLE_H

#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include <QFlags>
#include <QRectF>

#include "core/map_color.h"
#include "render/render_ir.h"

class QColor;

namespace OpenOrienteering {

class Map;
class Object;

namespace render {
class MapRenderSnapshot;
class OverlaySceneBuilder;
struct RenderRequest;
struct SnapshotObjectBlock;
struct SnapshotObject;
}

/** View-dependent policy used while producing ordered render IR. */
class RenderConfig
{
public:
	enum Option
	{
		Screen              = 1<<0,
		DisableAntialiasing = 1<<1,
		ForceMinSize        = 1<<2,
		HelperSymbols       = 1<<3,
		Highlighted         = 1<<4,
		RequireSpotColor    = 1<<5,
		Tool                = Screen | ForceMinSize | HelperSymbols,
		NoOptions           = 0
	};
	Q_DECLARE_FLAGS(Options, Option)

	const Map& map;
	QRectF bounding_box;
	qreal scaling;
	Options options;
	qreal opacity;

	bool testFlag(Option flag) const;
};

struct RenderPrimitiveConfig
{
	qreal scaling = 1;
	RenderConfig::Options options;
	render::Color color;
	render::ObjectId object_id = 0;
};

/**
 * Immutable backend-neutral geometry produced by a map symbol.
 *
 * Instances are explicitly shared by the live document, immutable snapshots,
 * and in-flight render work. They never contain QPainter or native resources.
 */
class Renderable
{
protected:
	explicit Renderable(const MapColor* color);

public:
	Renderable(const Renderable&) = delete;
	Renderable(Renderable&&) = delete;
	virtual ~Renderable();

	Renderable& operator=(const Renderable&) = delete;
	Renderable& operator=(Renderable&&) = delete;

	const QRectF& getExtent() const;
	bool intersects(const QRectF& rect) const;
	int colorPriority() const noexcept;

	virtual void appendTo(render::RenderIRBuilder& builder,
	                      const RenderPrimitiveConfig& config) const = 0;

protected:
	const int color_priority;
	QRectF extent;
};

struct RenderableItem
{
	std::shared_ptr<const Renderable> renderable;
	render::PathPtr clip_path;
};

using RenderableVector = std::vector<RenderableItem>;
using SharedRenderables = std::shared_ptr<const RenderableVector>;

/** Render geometry for one object, grouped only by map-color priority. */
class ObjectRenderables : protected std::map<int, std::shared_ptr<RenderableVector>>
{
	friend class MapRenderables;
	friend class render::MapRenderSnapshot;

public:
	explicit ObjectRenderables(Object& object);
	ObjectRenderables(const ObjectRenderables&) = delete;
	ObjectRenderables& operator=(const ObjectRenderables&) = delete;
	~ObjectRenderables();

	void insertRenderable(Renderable* renderable);
	void clear();
	void detach();
	std::shared_ptr<const render::RenderIR> buildIR(
		int map_color, render::Color color, const render::RenderRequest& request
	) const;

	void setClipPath(render::PathPtr path);
	render::PathPtr getClipPath() const;
	const QRectF& getExtent() const;

private:
	std::shared_ptr<RenderableVector>& writableColor(int color_priority);

	QRectF& extent;
	render::PathPtr clip_path;
};

/**
 * Mutable publication boundary between the document and immutable snapshots.
 *
 * The object pointers below are used only on the document thread while a new
 * snapshot is assembled. Published snapshots contain stable ids, values, and
 * shared immutable geometry—not pointers back into the Map.
 */
class MapRenderables
{
public:
	class ObjectDeleter
	{
	public:
		MapRenderables& renderables;
		void operator()(Object* object) const;
	};

	explicit MapRenderables(Map* map);

	void draw(render::OverlaySceneBuilder* painter, const RenderConfig& config) const;

	void insertRenderablesOfObject(const Object* object);
	void removeRenderablesOfObject(const Object* object, bool mark_area_as_dirty);
	void clear(bool mark_area_as_dirty = false);
	void invalidate();

	std::shared_ptr<const render::MapRenderSnapshot> snapshot() const;
	render::Revision revision() const noexcept;
	bool empty() const;

private:
	struct ObjectEntry
	{
		render::ObjectId id = 0;
		std::map<int, SharedRenderables> colors;
		mutable std::shared_ptr<const render::SnapshotObject> snapshot;
	};

	void changed();
	void addToColorOrder(const ObjectEntry& entry);
	void removeFromColorOrder(const ObjectEntry& entry);
	void markObjectBlockDirty(render::ObjectId id);

	Map* const map;
	std::map<const Object*, ObjectEntry> objects;
	std::map<render::ObjectId, const Object*> object_order;
	std::map<int, std::set<render::ObjectId>> color_object_ids;
	render::ObjectId next_object_id = 1;
	render::Revision current_revision = 1;
	mutable std::vector<std::shared_ptr<const render::SnapshotObjectBlock>> snapshot_blocks;
	mutable std::set<std::size_t> dirty_blocks;
	mutable std::map<int, std::shared_ptr<const std::vector<render::ObjectId>>> snapshot_color_objects;
	mutable std::set<int> dirty_color_priorities;
	mutable std::shared_ptr<const render::MapRenderSnapshot> published_snapshot;
};

inline bool RenderConfig::testFlag(const RenderConfig::Option flag) const
{
	return options.testFlag(flag);
}

inline Renderable::Renderable(const MapColor* color)
 : color_priority(color ? color->getPriority() : MapColor::Reserved)
{
	// nothing
}

inline const QRectF& Renderable::getExtent() const
{
	return extent;
}

inline bool Renderable::intersects(const QRectF& rect) const
{
	return extent.intersects(rect);
}

inline int Renderable::colorPriority() const noexcept
{
	return color_priority;
}

inline render::PathPtr ObjectRenderables::getClipPath() const
{
	return clip_path;
}

inline const QRectF& ObjectRenderables::getExtent() const
{
	return extent;
}

inline render::Revision MapRenderables::revision() const noexcept
{
	return current_revision;
}

inline bool MapRenderables::empty() const
{
	return objects.empty();
}

}  // namespace OpenOrienteering

Q_DECLARE_OPERATORS_FOR_FLAGS(OpenOrienteering::RenderConfig::Options)

#endif
