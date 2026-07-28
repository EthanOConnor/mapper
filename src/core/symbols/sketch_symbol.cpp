/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "core/symbols/sketch_symbol.h"

#include <algorithm>

#include <QPainterPath>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "core/map.h"
#include "core/map_coord.h"
#include "core/objects/object.h"
#include "core/renderables/renderable.h"
#include "core/virtual_coord_vector.h"
#include "core/virtual_path.h"
#include "render/qt_render_bridge.h"
#include "util/xml_stream_util.h"

namespace OpenOrienteering {

namespace {

constexpr auto minimum_width_native = 50;
constexpr auto maximum_width_native = 3000;

class SketchRenderable final : public Renderable
{
public:
	SketchRenderable(const SketchSymbol& symbol, const VirtualPath& virtual_path,
	                 bool closed)
	 : Renderable(Map::getSketchColor())
	 , stroke_color(symbol.strokeColor())
	 , line_width(0.001 * symbol.getLineWidth())
	{
		if (virtual_path.size() < 2)
			return;

		const auto& flags = virtual_path.coords.flags;
		const auto& coords = virtual_path.coords;
		QPainterPath painter_path;
		auto index = virtual_path.first_index;
		painter_path.moveTo(coords[index]);
		for (++index; index <= virtual_path.last_index; ++index)
		{
			if (flags[index - 1].isCurveStart()
			    && index + 2 <= virtual_path.last_index)
			{
				painter_path.cubicTo(
				    coords[index], coords[index + 1], coords[index + 2]);
				index += 2;
			}
			else if (flags[index].isGapPoint() || flags[index].isHolePoint())
			{
				painter_path.moveTo(coords[index]);
			}
			else
			{
				painter_path.lineTo(coords[index]);
			}
		}
		if (closed)
			painter_path.closeSubpath();

		auto const margin = 0.5 * line_width;
		extent = painter_path.boundingRect().adjusted(
		    -margin, -margin, margin, margin);
		path = render::fromQPainterPath(painter_path);
	}

	void appendTo(render::RenderIRBuilder& builder,
	              const RenderPrimitiveConfig& config) const override
	{
		if (!path || !stroke_color.isValid())
			return;
		auto width = line_width;
		if (config.options.testFlag(RenderConfig::ForceMinSize)
		    && config.scaling > 0)
			width = std::max(width, 1.25 / config.scaling);
		builder.strokePath(
		    path, render::fromQColor(stroke_color),
		    { .width = width,
		      .cap = render::LineCap::Round,
		      .join = render::LineJoin::Round,
		      .miter_limit = 1,
		      .dash_pattern = {},
		      .dash_offset = 0 },
		    render::QualityHint::ForceAntialiasing);
	}

private:
	QColor stroke_color;
	qreal line_width = 0;
	render::PathPtr path;
};

}  // namespace

SketchSymbol::SketchSymbol()
{
	setLineWidth(0.35);
	setCapStyle(LineSymbol::RoundCap);
	setJoinStyle(LineSymbol::RoundJoin);
	setIsHelperSymbol(true);
	setProtected(true);
}

SketchSymbol::SketchSymbol(const SketchSymbol& proto)
 : LineSymbol(proto)
 , stroke_color(proto.stroke_color)
{
}

SketchSymbol::~SketchSymbol() = default;

SketchSymbol* SketchSymbol::duplicate() const
{
	return new SketchSymbol(*this);
}

const QColor& SketchSymbol::strokeColor() const noexcept
{
	return stroke_color;
}

void SketchSymbol::setStrokeColor(const QColor& color)
{
	if (color.isValid())
		stroke_color = color;
}

void SketchSymbol::createRenderables(
        const Object* /*object*/, const VirtualCoordVector& coords,
        ObjectRenderables& output, RenderableOptions /*options*/) const
{
	const auto parts = PathPart::calculatePathParts(coords);
	for (const auto& part : parts)
	{
		if (part.size() >= 2)
			output.insertRenderable(
			    new SketchRenderable(*this, part, part.isClosed()));
	}
}

void SketchSymbol::createRenderables(
        const PathObject* /*object*/, const PathPartVector& path_parts,
        ObjectRenderables& output, RenderableOptions /*options*/) const
{
	for (const auto& part : path_parts)
	{
		if (part.size() >= 2)
			output.insertRenderable(
			    new SketchRenderable(*this, part, part.isClosed()));
	}
}

void SketchSymbol::saveRootAttributes(QXmlStreamWriter& xml) const
{
	xml.writeAttribute(
	    QStringLiteral("kind"), QStringLiteral("mapper-sketch-v1"));
}

void SketchSymbol::saveImpl(QXmlStreamWriter& xml, const Map& /*map*/) const
{
	XmlElementWriter element(xml, QLatin1String("sketch_symbol"));
	element.writeAttribute(
	    QLatin1String("color"), stroke_color.name(QColor::HexArgb));
	element.writeAttribute(QLatin1String("width"), getLineWidth());
}

bool SketchSymbol::loadImpl(
        QXmlStreamReader& xml, const Map& /*map*/,
        SymbolDictionary& /*symbol_dict*/, int /*version*/)
{
	if (xml.name() != QLatin1String("sketch_symbol"))
		return false;
	XmlElementReader element(xml);
	const auto color =
	    QColor(element.attribute<QString>(QLatin1String("color")));
	if (color.isValid())
		stroke_color = color;
	const auto width = std::clamp(
	    element.attribute<int>(QLatin1String("width")),
	    minimum_width_native, maximum_width_native);
	setLineWidth(0.001 * width);
	setCapStyle(LineSymbol::RoundCap);
	setJoinStyle(LineSymbol::RoundJoin);
	setIsHelperSymbol(true);
	setProtected(true);
	return true;
}

bool SketchSymbol::equalsImpl(
        const Symbol* other, Qt::CaseSensitivity case_sensitivity) const
{
	const auto* sketch = dynamic_cast<const SketchSymbol*>(other);
	return sketch
	    && LineSymbol::equalsImpl(other, case_sensitivity)
	    && sketch->stroke_color.rgba64() == stroke_color.rgba64()
	    && sketch->getLineWidth() == getLineWidth();
}

}  // namespace OpenOrienteering
