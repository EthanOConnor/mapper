/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2012-2017 Kai Pastor
 *
 *    This file is part of OpenOrienteering.
 *	
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "renderable_implementation.h"

#include <cstddef>
#include <iterator>
#include <memory>
#include <vector>

#include <QtMath>
#include <QtNumeric>
#include <QFont>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QTransform>
// IWYU pragma: no_include <QVariant>

#include "core/map_coord.h"
#include "core/virtual_coord_vector.h"
#include "core/virtual_path.h"
#include "core/objects/object.h"
#include "core/objects/text_object.h"
#include "core/renderables/renderable.h"
#include "core/symbols/area_symbol.h"
#include "core/symbols/line_symbol.h"
#include "core/symbols/point_symbol.h"
#include "core/symbols/text_symbol.h"
#include "render/qt_render_bridge.h"
#include "util/util.h"

// IWYU pragma: no_forward_declare QFontMetricsF


namespace OpenOrienteering {

// ### DotRenderable ###

DotRenderable::DotRenderable(const PointSymbol* symbol, MapCoordF coord)
 : Renderable(symbol->getInnerColor())
{
	double x = coord.x();
	double y = coord.y();
	double radius = (0.001 * symbol->getInnerRadius());
	extent = QRectF(x - radius, y - radius, 2 * radius, 2 * radius);
}

void DotRenderable::appendTo(render::RenderIRBuilder& builder,
	                         const RenderPrimitiveConfig& config) const
{
	if (config.options.testFlag(RenderConfig::ForceMinSize) && extent.width() * config.scaling < 1.5)
	{
		auto const center = extent.center();
		auto const radius = 0.5 / config.scaling;
		builder.fillEllipse(
			{ center.x() - radius, center.y() - radius, 2 * radius, 2 * radius },
			config.color,
			config.object_id
		);
	}
	else
		builder.fillEllipse(render::fromQRectF(extent), config.color, config.object_id);
}



// ### CircleRenderable ###

CircleRenderable::CircleRenderable(const PointSymbol* symbol, MapCoordF coord)
 : Renderable(symbol->getOuterColor())
 , line_width(0.001 * symbol->getOuterWidth())
{
	double x = coord.x();
	double y = coord.y();
	double radius = (0.001 * symbol->getInnerRadius()) + line_width/2;
	rect = QRectF(x - radius, y - radius, 2 * radius, 2 * radius);
	extent = QRectF(rect.x() - 0.5*line_width, rect.y() - 0.5*line_width, rect.width() + line_width, rect.height() + line_width);
}

void CircleRenderable::appendTo(render::RenderIRBuilder& builder,
	                            const RenderPrimitiveConfig& config) const
{
	auto width = line_width;
	if (color_priority < 0 && color_priority != MapColor::Registration)
		width /= config.scaling;
	else if (config.options.testFlag(RenderConfig::ForceMinSize) && width * config.scaling < 1)
		width = 0;

	if (config.options.testFlag(RenderConfig::Screen) && line_width * config.scaling < 0.125)
		return;

	auto bounds = render::fromQRectF(rect);
	if (config.options.testFlag(RenderConfig::ForceMinSize) && rect.width() * config.scaling < 1.5)
	{
		auto const center = rect.center();
		auto const radius = 0.5 / config.scaling;
		bounds = { center.x() - radius, center.y() - radius, 2 * radius, 2 * radius };
	}
	builder.strokeEllipse(bounds, config.color,
	                      { .width = width, .dash_pattern = {}, .dash_offset = 0 },
	                      config.object_id,
	                      color_priority < 0 ? render::QualityHint::ForceAntialiasing
	                                         : render::QualityHint::Default);
}



// ### LineRenderable ###

LineRenderable::LineRenderable(const LineSymbol* symbol, const VirtualPath& virtual_path, bool closed)
 : Renderable(symbol->getColor())
 , line_width(0.001 * symbol->getLineWidth())
{
	Q_ASSERT(virtual_path.size() >= 2);
	QPainterPath path;
	
	qreal half_line_width = (color_priority < 0) ? 0 : line_width/2;
	
	switch (symbol->getCapStyle())
	{
		case LineSymbol::FlatCap:		cap_style = render::LineCap::Flat;	break;
		case LineSymbol::RoundCap:		cap_style = render::LineCap::Round;	break;
		case LineSymbol::SquareCap:		cap_style = render::LineCap::Square;	break;
		case LineSymbol::PointedCap:	cap_style = render::LineCap::Flat;	break;
	}
	switch (symbol->getJoinStyle())
	{
		case LineSymbol::BevelJoin:		join_style = render::LineJoin::Bevel;	break;
		case LineSymbol::MiterJoin:		join_style = render::LineJoin::Miter;	break;
		case LineSymbol::RoundJoin:		join_style = render::LineJoin::Round;	break;
	}
	
	auto& flags  = virtual_path.coords.flags;
	auto& coords = virtual_path.coords;
	
	bool has_curve = false;
	bool hole = false;
	QPainterPath first_subpath;
	
	auto i = virtual_path.first_index;
	bool gap = flags[i].isGapPoint();  // Line may start with a gap
	path.moveTo(coords[i]);
	extent = QRectF(coords[i].x(), coords[i].y(), 0.0001, 0.0001);
	extentIncludeCap(i, half_line_width, false, symbol, virtual_path);
	
	for (++i; i <= virtual_path.last_index; ++i)
	{
		if (gap)
		{
			if (flags[i].isHolePoint())
			{
				gap = false;
				hole = true;
			}
			else if (flags[i].isGapPoint())
			{
				gap = false;
				if (first_subpath.isEmpty() && closed)
				{
					first_subpath = path;
					path = QPainterPath();
				}
				path.moveTo(coords[i]);
				extentIncludeCap(i, half_line_width, false, symbol, virtual_path);
			}
			continue;
		}
		
		if (hole)
		{
			Q_ASSERT(!flags[i].isHolePoint() && "Two hole points in a row!");
			if (first_subpath.isEmpty() && closed)
			{
				first_subpath = path;
				path = QPainterPath();
			}
			path.moveTo(coords[i]);
			extentIncludeCap(i, half_line_width, false, symbol, virtual_path);
			hole = false;
			continue;
		}
		
		if (flags[i-1].isCurveStart())
		{
			Q_ASSERT(i < virtual_path.last_index-1);
			has_curve = true;
			path.cubicTo(coords[i], coords[i+1], coords[i+2]);
			i += 2;
		}
		else
			path.lineTo(coords[i]);
		
		if (flags[i].isHolePoint())
			hole = true;
		else if (flags[i].isGapPoint())
			gap = true;
		
		if ((i < virtual_path.last_index && !hole && !gap) || (i == virtual_path.last_index && closed))
			extentIncludeJoin(i, half_line_width, symbol, virtual_path);
		else
			extentIncludeCap(i, half_line_width, true, symbol, virtual_path);
	}
	
	if (closed)
	{
		if (first_subpath.isEmpty())
			path.closeSubpath();
		else
			path.connectPath(first_subpath);
	}
	
	// If we do not have the path coords, but there was a curve, calculate path coords.
	if (has_curve)
	{
		//  This happens for point symbols with curved lines in them.
		const auto& path_coords = virtual_path.path_coords;
		Q_ASSERT(path_coords.front().param == 0.0f);
		Q_ASSERT(path_coords.back().param == 0.0f);
		for (auto i = path_coords.size()-1; i > 0; --i)
		{
			if (path_coords[i].param != 0.0f)
			{
				const auto& pos = path_coords[i].pos;
				auto to_coord   = pos - path_coords[i-1].pos;
				auto to_next    = path_coords[i+1].pos - pos;
				to_coord.normalize();
				to_next.normalize();
				auto right = (to_coord + to_next).perpRight();
				right.setLength(half_line_width);
				
				rectInclude(extent, pos + right);
				rectInclude(extent, pos - right);
			}
		}
	}
	this->path = render::fromQPainterPath(path);
	Q_ASSERT(extent.right() < 60000000);	// assert if bogus values are returned
}

LineRenderable::LineRenderable(const LineSymbol* symbol, QPointF first, QPointF second)
 : Renderable(symbol->getColor())
 , line_width(0.001 * symbol->getLineWidth())
 , cap_style(render::LineCap::Flat)
 , join_style(render::LineJoin::Miter)
{
	qreal half_line_width = (color_priority < 0) ? 0 : line_width/2;
	
	auto margin = MapCoordF(second - first).perpRight();
	margin.normalize();
	margin.rx() = qAbs(margin.x()) * half_line_width;
	margin.ry() = qAbs(margin.y()) * half_line_width;
	extent = QRectF(first, second)
	             .normalized()
	             .adjusted(-margin.x(), -margin.y(), margin.x(), margin.y());
	
	render::PathBuilder builder;
	builder.moveTo({ first.x(), first.y() });
	builder.lineTo({ second.x(), second.y() });
	path = builder.finish();
}

void LineRenderable::extentIncludeCap(quint32 i, qreal half_line_width, bool end_cap, const LineSymbol* symbol, const VirtualPath& path)
{
	const auto& coord = path.coords[i];
	if (half_line_width < 0.0005)
	{
		rectInclude(extent, coord);
		return;
	}
	
	if (symbol->getCapStyle() == LineSymbol::RoundCap)
	{
		rectInclude(extent, QPointF(coord.x() - half_line_width, coord.y() - half_line_width));
		rectInclude(extent, QPointF(coord.x() + half_line_width, coord.y() + half_line_width));
		return;
	}
	
	auto right = path.calculateTangent(i).perpRight();
	right.normalize();
	rectInclude(extent, coord + half_line_width * right);
	rectInclude(extent, coord - half_line_width * right);
	
	if (symbol->getCapStyle() == LineSymbol::SquareCap)
	{
		auto back = right.perpRight();
		if (end_cap)
		    back = -back;
		rectInclude(extent, coord + half_line_width * (back - right));
		rectInclude(extent, coord + half_line_width * (back + right));
	}
}

void LineRenderable::extentIncludeJoin(quint32 i, qreal half_line_width, const LineSymbol* symbol, const VirtualPath& path)
{
	const auto& coord = path.coords[i];
	if (half_line_width < 0.0005)
	{
		rectInclude(extent, coord);
		return;
	}
	
	if (symbol->getJoinStyle() == LineSymbol::RoundJoin)
	{
		rectInclude(extent, QPointF(coord.x() - half_line_width, coord.y() - half_line_width));
		rectInclude(extent, QPointF(coord.x() + half_line_width, coord.y() + half_line_width));
		return;
	}
	
	bool ok_to_coord, ok_to_next;
	MapCoordF to_coord = path.calculateIncomingTangent(i, ok_to_coord);
	MapCoordF to_next = path.calculateOutgoingTangent(i, ok_to_next);
	if (!ok_to_next)
	{
		if (!ok_to_coord)
			return;
		to_next = to_coord;
	}
	else if (!ok_to_coord)
	{
		to_coord = to_next;
	}
	
	auto r0 = to_coord.perpRight();
	r0.setLength(half_line_width);
	auto r1 = to_next.perpRight();
	r1.setLength(half_line_width);
	
	auto to_coord_rhs = coord + r0;
	auto to_coord_lhs = coord - r0;
	auto to_next_rhs  = coord + r1;
	auto to_next_lhs  = coord - r1;
	if (symbol->getJoinStyle() == LineSymbol::BevelJoin)
	{
		rectInclude(extent, to_coord_rhs);
		rectInclude(extent, to_coord_lhs);
		rectInclude(extent, to_next_rhs);
		rectInclude(extent, to_next_lhs);
		return;
	}
	
	auto limit = line_width * LineSymbol::miterLimit();
	to_coord.setLength(limit);
	to_next.setLength(limit);
	
	const auto scaling = to_coord.y() * to_next.x() - to_coord.x() * to_next.y();
	if (qIsNull(scaling) || !qIsFinite(scaling))
	{
		// straight line or 180 degrees turn
		if (to_coord == -to_next)
		{
			switch (symbol->getJoinStyle()) {
			case LineSymbol::MiterJoin:
				to_coord.setLength(2*half_line_width);
				break;
			case LineSymbol::RoundJoin:
				to_coord.setLength(half_line_width);
				break;
			case LineSymbol::BevelJoin:
				to_coord.setLength(0);
				break;
			}
			rectInclude(extent, coord + to_coord);
		}
		return;
	}
	
	// rhs boundary
	auto p = to_coord_rhs - to_next_rhs;
	auto factor = (to_next.y() * p.x() - to_next.x() * p.y()) / scaling;
	if (factor > 1)
	{
		// outer boundary, intersection exceeds miter limit
		rectInclude(extent, to_coord_rhs + to_coord);
		rectInclude(extent, to_next_rhs - to_next);
	}
	else if (factor > 0)
	{
		// outer boundary, intersection within miter limit
		rectInclude(extent, to_coord_rhs + to_coord * factor);
	}
	else
	{
		// inner boundary
		rectInclude(extent, to_coord_rhs);
		rectInclude(extent, to_next_rhs);
	}
	
	// lhs boundary
	p = to_coord_lhs - to_next_lhs;
	factor = (to_next.y() * p.x() - to_next.x() * p.y()) / scaling;
	if (factor > 1)
	{
		// outer boundary, intersection exceeds miter limit
		rectInclude(extent, to_coord_lhs + to_coord);
		rectInclude(extent, to_next_lhs - to_next);
	}
	else if (factor > 0)
	{
		// outer boundary, intersection within miter limit
		rectInclude(extent, to_coord_lhs + to_coord * factor);
	}
	else
	{
		// inner boundary, catch rare cases
		rectInclude(extent, to_coord_lhs);
		rectInclude(extent, to_next_lhs);
	}
}

void LineRenderable::appendTo(render::RenderIRBuilder& builder,
	                          const RenderPrimitiveConfig& config) const
{
	auto width = line_width;
	if (color_priority < 0 && color_priority != MapColor::Registration)
		width /= config.scaling;
	else if (config.options.testFlag(RenderConfig::ForceMinSize) && width * config.scaling < 1)
		width = 0;

	if (config.options.testFlag(RenderConfig::Screen) && line_width * config.scaling < 0.125)
		return;

	builder.strokePath(
		path,
		config.color,
		{
			.width = width,
			.cap = cap_style,
			.join = join_style,
			.miter_limit = LineSymbol::miterLimit(),
			.dash_pattern = {},
			.dash_offset = 0,
		},
		config.object_id,
		color_priority < 0 ? render::QualityHint::ForceAntialiasing
		                   : render::QualityHint::Default
	);
}

// ### AreaRenderable ###

AreaRenderable::AreaRenderable(const AreaSymbol* symbol, const PathPartVector& path_parts)
 : Renderable(symbol->getColor())
{
	render::PathBuilder builder;
	if (!path_parts.empty())
	{
		auto part = begin(path_parts);
		if (part->size() > 2)
		{
			extent = part->path_coords.calculateExtent();
			addSubpath(*part, builder);
			
			auto last = end(path_parts);
			for (++part; part != last; ++part)
			{
				rectInclude(extent, part->path_coords.calculateExtent());
				addSubpath(*part, builder);
			}
		}
	}
	path = builder.finish();
	Q_ASSERT(extent.right() < 60000000);	// assert if bogus values are returned
}

AreaRenderable::AreaRenderable(const AreaSymbol* symbol, const VirtualPath& path)
 : Renderable(symbol->getColor())
{
	extent = path.path_coords.calculateExtent();
	render::PathBuilder builder;
	addSubpath(path, builder);
	this->path = builder.finish();
}

void AreaRenderable::addSubpath(const VirtualPath& virtual_path, render::PathBuilder& builder)
{
	auto& flags  = virtual_path.coords.flags;
	auto& coords = virtual_path.coords;
	Q_ASSERT(!flags.data().empty());
	
	auto i = virtual_path.first_index;
	builder.moveTo({ coords[i].x(), coords[i].y() });
	for (++i; i <= virtual_path.last_index; ++i)
	{
		if (flags[i-1].isCurveStart())
		{
			Q_ASSERT(i+2 < coords.size());
			builder.cubicTo(
				{ coords[i].x(), coords[i].y() },
				{ coords[i+1].x(), coords[i+1].y() },
				{ coords[i+2].x(), coords[i+2].y() }
			);
			i += 2;
		}
		else
		{
			builder.lineTo({ coords[i].x(), coords[i].y() });
		}
	}
	builder.close();
}

void AreaRenderable::appendTo(render::RenderIRBuilder& builder,
	                          const RenderPrimitiveConfig& config) const
{
	builder.fillPath(path, config.color, config.object_id);
}



// ### TextRenderable ###

TextRenderable::TextRenderable(const TextSymbol* symbol, const TextObject* text_object, const MapColor* color, double anchor_x, double anchor_y)
: Renderable { color }
{
	QPainterPath painter_path;
	painter_path.setFillRule(Qt::WindingFill); // Otherwise intersecting text and underlines create holes.
	scale_factor = symbol->getFontSize() / TextSymbol::internal_point_size;
	
	const QFont& font(symbol->getQFont());
	const QFontMetricsF& metrics(symbol->getFontMetrics());
	
	int num_lines = text_object->getNumLines();
	for (int i=0; i < num_lines; i++)
	{
		const TextObjectLineInfo* line_info = text_object->getLineInfo(i);
		
		double line_y = line_info->line_y;
		
		double underline_x0 = 0.0;
		double underline_y0 = line_info->line_y + metrics.underlinePos();
		double underline_y1 = underline_y0 + metrics.lineWidth();
		
		auto num_parts = line_info->part_infos.size();
		for (std::size_t j=0; j < num_parts; j++)
		{
			const TextObjectPartInfo& part(line_info->part_infos.at(j));
			if (font.underline())
			{
				if (j > 0)
				{
					// draw underline for gap between parts as rectangle
					// TODO: watch out for inconsistency between text and gap underline
					painter_path.moveTo(underline_x0, underline_y0);
					painter_path.lineTo(part.part_x,  underline_y0);
					painter_path.lineTo(part.part_x,  underline_y1);
					painter_path.lineTo(underline_x0, underline_y1);
					painter_path.closeSubpath();
				}
				underline_x0 = part.part_x;
			}
			painter_path.addText(part.part_x, line_y, font, part.part_text);
		}
	}
	
	QTransform t { 1.0, 0.0, 0.0, 1.0, anchor_x, anchor_y };
	t.scale(scale_factor, scale_factor);
	
	auto rotation_rad = text_object->getRotation();
	if (!qIsNull(rotation_rad))
	{
		t.rotate(-qRadiansToDegrees(rotation_rad));
	}
	
	extent = t.mapRect(painter_path.controlPointRect());
	path = render::fromQPainterPath(painter_path);
	transform = render::fromQTransform(t);
}

void TextRenderable::appendTo(render::RenderIRBuilder& builder,
	                          const RenderPrimitiveConfig& config) const
{
	builder.pushTransform(transform);
	builder.fillPath(path, config.color, config.object_id, render::QualityHint::Text);
	builder.popTransform();
}



// ### TextRenderable ###

TextFramingRenderable::TextFramingRenderable(const TextSymbol* symbol, const TextObject* text_object, const MapColor* color, double anchor_x, double anchor_y)
: TextRenderable     { symbol, text_object, color, anchor_x, anchor_y }
, framing_line_width { 2 * 0.001 * symbol->getFramingLineHalfWidth() / scale_factor }
{
	auto adjustment = 0.001 * symbol->getFramingLineHalfWidth() ;
	extent.adjust(-adjustment, -adjustment, +adjustment, +adjustment);
}

void TextFramingRenderable::appendTo(render::RenderIRBuilder& builder,
	                                 const RenderPrimitiveConfig& config) const
{
	auto width = framing_line_width;
	if (config.options.testFlag(RenderConfig::ForceMinSize)
	    && width * scale_factor * config.scaling < 1)
		width = 0;
	if (config.options.testFlag(RenderConfig::Screen)
	    && framing_line_width * scale_factor * config.scaling < 0.125)
		return;
	builder.pushTransform(transform);
	builder.strokePath(
		path,
		config.color,
		{
			.width = width,
			.cap = render::LineCap::Flat,
			.join = render::LineJoin::Miter,
			.miter_limit = 0.5,
			.dash_pattern = {},
			.dash_offset = 0,
		},
		config.object_id,
		render::QualityHint::Text
	);
	builder.popTransform();
}


}  // namespace OpenOrienteering
