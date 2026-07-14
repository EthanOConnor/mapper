/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/qt_render_bridge.h"

#include <cmath>

#include <QColor>
#include <QPainterPath>
#include <QRectF>
#include <QTransform>

namespace OpenOrienteering::render {

Rect fromQRectF(const QRectF& rect)
{
	if (!rect.isValid())
		return {};
	return { rect.x(), rect.y(), rect.width(), rect.height() };
}

QRectF toQRectF(const Rect& rect)
{
	return { rect.x, rect.y, rect.width, rect.height };
}

Color fromQColor(const QColor& color)
{
	auto const rgba = color.rgba64();
	return { rgba.red(), rgba.green(), rgba.blue(), rgba.alpha() };
}

QColor toQColor(const Color& color)
{
	return QColor::fromRgba64(color.red, color.green, color.blue, color.alpha);
}

Transform fromQTransform(const QTransform& transform)
{
	return {
		transform.m11(), transform.m12(),
		transform.m21(), transform.m22(),
		transform.dx(), transform.dy()
	};
}

QTransform toQTransform(const Transform& transform)
{
	return {
		transform.m11, transform.m12,
		transform.m21, transform.m22,
		transform.dx, transform.dy
	};
}

PathPtr fromQPainterPath(const QPainterPath& path)
{
	PathBuilder builder(path.fillRule() == Qt::WindingFill ? FillRule::Winding : FillRule::OddEven);
	Point subpath_start;
	bool has_subpath = false;

	for (int i = 0; i < path.elementCount(); ++i)
	{
		auto const element = path.elementAt(i);
		switch (element.type)
		{
		case QPainterPath::MoveToElement:
			subpath_start = { element.x, element.y };
			has_subpath = true;
			builder.moveTo(subpath_start);
			break;
		case QPainterPath::LineToElement:
		{
			Point const point { element.x, element.y };
			auto const at_subpath_end = i + 1 == path.elementCount()
			                            || path.elementAt(i + 1).type == QPainterPath::MoveToElement;
			if (has_subpath && at_subpath_end
			    && point.x == subpath_start.x && point.y == subpath_start.y)
			{
				builder.close();
			}
			else
			{
				builder.lineTo(point);
			}
			break;
		}
		case QPainterPath::CurveToElement:
			if (i + 2 < path.elementCount())
			{
				auto const control_2 = path.elementAt(i + 1);
				auto const end = path.elementAt(i + 2);
				builder.cubicTo(
					{ element.x, element.y },
					{ control_2.x, control_2.y },
					{ end.x, end.y }
				);
				i += 2;
			}
			break;
		case QPainterPath::CurveToDataElement:
			break;
		}
	}

	return builder.finish();
}

QPainterPath toQPainterPath(const Path& path)
{
	QPainterPath result;
	result.setFillRule(path.fillRule() == FillRule::Winding ? Qt::WindingFill : Qt::OddEvenFill);
	for (auto const& element : path.elements())
	{
		switch (element.verb)
		{
		case PathVerb::MoveTo:
			result.moveTo(element.points[0].x, element.points[0].y);
			break;
		case PathVerb::LineTo:
			result.lineTo(element.points[0].x, element.points[0].y);
			break;
		case PathVerb::CubicTo:
			result.cubicTo(
				element.points[0].x, element.points[0].y,
				element.points[1].x, element.points[1].y,
				element.points[2].x, element.points[2].y
			);
			break;
		case PathVerb::Close:
			result.closeSubpath();
			break;
		}
	}
	return result;
}

}  // namespace OpenOrienteering::render
