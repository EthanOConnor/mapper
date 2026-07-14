/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2012-2017 Kai Pastor
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_RENDERABLE_IMPLEMENTATION_H
#define OPENORIENTEERING_RENDERABLE_IMPLEMENTATION_H

#include <QRectF>

#include "core/renderables/renderable.h"
#include "render/render_ir.h"

class QPointF;

namespace OpenOrienteering {

class AreaSymbol;
class LineSymbol;
class MapColor;
class MapCoordF;
class PathPartVector;
class PointSymbol;
class TextObject;
class TextSymbol;
class VirtualPath;

class DotRenderable : public Renderable
{
public:
	DotRenderable(const PointSymbol* symbol, MapCoordF coord);
	void appendTo(render::RenderIRBuilder& builder,
	              const RenderPrimitiveConfig& config) const override;
};

class CircleRenderable : public Renderable
{
public:
	CircleRenderable(const PointSymbol* symbol, MapCoordF coord);
	void appendTo(render::RenderIRBuilder& builder,
	              const RenderPrimitiveConfig& config) const override;

private:
	qreal line_width;
	QRectF rect;
};

class LineRenderable : public Renderable
{
public:
	LineRenderable(const LineSymbol* symbol, const VirtualPath& virtual_path, bool closed);
	LineRenderable(const LineSymbol* symbol, QPointF first, QPointF second);
	void appendTo(render::RenderIRBuilder& builder,
	              const RenderPrimitiveConfig& config) const override;

private:
	void extentIncludeCap(quint32 i, qreal half_line_width, bool end_cap,
	                      const LineSymbol* symbol, const VirtualPath& path);
	void extentIncludeJoin(quint32 i, qreal half_line_width,
	                       const LineSymbol* symbol, const VirtualPath& path);

	qreal line_width;
	render::PathPtr path;
	render::LineCap cap_style = render::LineCap::Flat;
	render::LineJoin join_style = render::LineJoin::Miter;
};

class AreaRenderable : public Renderable
{
public:
	AreaRenderable(const AreaSymbol* symbol, const PathPartVector& path_parts);
	AreaRenderable(const AreaSymbol* symbol, const VirtualPath& path);
	void appendTo(render::RenderIRBuilder& builder,
	              const RenderPrimitiveConfig& config) const override;

	const render::PathPtr& renderPath() const noexcept;

private:
	void addSubpath(const VirtualPath& virtual_path, render::PathBuilder& builder);

	render::PathPtr path;
};

class TextRenderable : public Renderable
{
public:
	TextRenderable(const TextSymbol* symbol, const TextObject* text_object,
	               const MapColor* color, double anchor_x, double anchor_y);
	void appendTo(render::RenderIRBuilder& builder,
	              const RenderPrimitiveConfig& config) const override;

protected:
	render::PathPtr path;
	render::Transform transform;
	double scale_factor = 1;
};

class TextFramingRenderable : public TextRenderable
{
public:
	TextFramingRenderable(const TextSymbol* symbol, const TextObject* text_object,
	                      const MapColor* color, double anchor_x, double anchor_y);
	void appendTo(render::RenderIRBuilder& builder,
	              const RenderPrimitiveConfig& config) const override;

private:
	qreal framing_line_width;
};

inline const render::PathPtr& AreaRenderable::renderPath() const noexcept
{
	return path;
}

}  // namespace OpenOrienteering

#endif
