/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2014, 2015, 2017 Kai Pastor
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

#include "point_handles.h"

#include <cmath>
#include <vector>

#include <Qt>
#include <QtGlobal>
#include <QBrush>
#include <QColor>
#include <QPainterPath>
#include <QPen>
#include <QPoint>

#include "core/objects/object.h"
#include "core/objects/text_object.h"
#include "gui/map/map_widget.h"


namespace OpenOrienteering {

PointHandles::PointHandles() noexcept
{
	// nothing
}


PointHandles::PointHandles(unsigned int factor)
: scale_factor(factor)
{
	display_radius = 6 * int(scale_factor);
}



void PointHandles::setScaleFactor(unsigned int factor)
{
	scale_factor = factor;
	display_radius = 6 * int(scale_factor); // = (image width + 1) / 2
}



// static
QRgb PointHandles::stateColor(PointHandleState state)
{
	switch (state)
	{
	case NormalHandleState:    return qRgb(  0,   0, 255);
	case ActiveHandleState:    return qRgb(255, 150,   0);
	case SelectedHandleState:  return qRgb(255,   0,   0);
	case DisabledHandleState:  return qRgb(106, 106, 106);
	}
	Q_UNREACHABLE();
	return qRgb(255,   0,   0);
}



void PointHandles::draw(
        render::OverlaySceneBuilder* painter,
        const MapWidget* widget,
        const Object* object,
        MapCoordVector::size_type hover_point,
        bool draw_curve_handles,
        PointHandleState base_state ) const
{
	MapCoordVector::size_type i = 0;
	switch (object->getType())
	{
	case Object::Point:
		draw(painter, widget->mapToViewport(object->asPoint()->getCoordF()), NormalHandle, (hover_point == 0) ? ActiveHandleState : base_state);
		return;
		
	case Object::Text:
		for (const auto& point : object->asText()->controlPoints())
		{
			draw(painter, widget->mapToViewport(point), NormalHandle, (hover_point == i) ? ActiveHandleState : base_state);
			i++;
		}
		return;
		
	case Object::Path:
		painter->setBrush(Qt::NoBrush); // for handle lines
		for (const auto& part : object->asPath()->parts())
		{
			const auto* path = part.path;

			bool have_curve = part.isClosed() && part.size() > 3 && path->getCoordinate(part.last_index - 3).isCurveStart();
			PointHandleType handle_type = NormalHandle;
			
			for (auto i = part.first_index; i <= part.last_index; ++i)
			{
				const MapCoord coord = path->getCoordinate(i);
				if (coord.isClosePoint())
					continue;
				QPointF point = widget->mapToViewport(coord);
				bool is_active = hover_point == i;
				
				if (i == part.first_index && !part.isClosed()) // || (i > part.start_index && path->getCoordinate(i-1).isHolePoint()))
					handle_type = StartHandle;
				else if (i == part.last_index && !part.isClosed()) // || coord.isHolePoint())
					handle_type = EndHandle;
				else
					handle_type = coord.isDashPoint() ? DashHandle : NormalHandle;
				
				// Draw incoming curve handle
				QPointF curve_handle;
				if (draw_curve_handles && have_curve)
				{
					auto curve_index = (i == part.first_index) ? (part.last_index - 1) : (i - 1);
					curve_handle = widget->mapToViewport(path->getCoordinate(curve_index));
					drawCurveHandleLine(painter, point, curve_handle, handle_type, is_active ? ActiveHandleState : base_state);
					draw(painter, curve_handle, CurveHandle, (is_active || hover_point == curve_index) ? ActiveHandleState : base_state);
					have_curve = false;
				}
				
				// Draw outgoing curve handle, first part
				if (draw_curve_handles && coord.isCurveStart())
				{
					curve_handle = widget->mapToViewport(path->getCoordinate(i+1));
					drawCurveHandleLine(painter, point, curve_handle,handle_type,  is_active ? ActiveHandleState : base_state);
				}
				
				// Draw point
				draw(painter, point, handle_type, is_active ? ActiveHandleState : base_state);
				
				// Draw outgoing curve handle, second part
				if (coord.isCurveStart())
				{
					if (draw_curve_handles)
					{
						draw(painter, curve_handle, CurveHandle, (is_active || hover_point == i + 1) ? ActiveHandleState : base_state);
						have_curve = true;
					}
					i += 2;
				}
			}
		}
		return;
	}
	
	Q_UNREACHABLE();
}


void PointHandles::draw(render::OverlaySceneBuilder* painter, const QPointF& position, PointHandleType type, PointHandleState state) const
{
	const auto factor = qreal(scale_factor > 0 ? scale_factor : 1);
	const QPointF center(qRound(position.x()), qRound(position.y()));
	const auto state_color = QColor::fromRgb(stateColor(state));
	const auto color_band = factor;
	const auto white_band = qMax<qreal>(1, factor / 2);

	auto square = [center](qreal radius) {
		QPainterPath path;
		path.addRect(QRectF(center.x() - radius, center.y() - radius,
		                    2 * radius, 2 * radius));
		return path;
	};
	auto circle = [center](qreal radius) {
		QPainterPath path;
		path.addEllipse(center, radius, radius);
		return path;
	};
	auto diamond = [center](qreal radius) {
		QPainterPath path;
		path.moveTo(center + QPointF(0, -radius));
		path.lineTo(center + QPointF(radius, 0));
		path.lineTo(center + QPointF(0, radius));
		path.lineTo(center + QPointF(-radius, 0));
		path.closeSubpath();
		return path;
	};
	auto draw_ring = [painter](QPainterPath outer, const QPainterPath& inner, const QColor& color) {
		outer.addPath(inner);
		outer.setFillRule(Qt::OddEvenFill);
		painter->setBrush(color);
		painter->drawPath(outer);
	};
	auto draw_classic_ring = [&](const auto& shape, qreal radius) {
		auto const color_inner = radius - color_band;
		auto const white_inner = color_inner - white_band;
		draw_ring(shape(radius), shape(color_inner), state_color);
		draw_ring(shape(color_inner), shape(white_inner), Qt::white);
	};

	painter->save();
	painter->setPen(Qt::NoPen);
	switch (type)
	{
	case StartHandle:
		draw_classic_ring(square, 5.5 * factor);
		break;
	case EndHandle:
	{
		const auto radius = 3.5 * factor;
		painter->setBrush(Qt::NoBrush);
		painter->setPen(QPen(state_color, 3 * factor, Qt::SolidLine, Qt::SquareCap, Qt::RoundJoin));
		painter->drawLine(center + QPointF(-radius, -radius), center + QPointF(radius, radius));
		painter->drawLine(center + QPointF(-radius, radius), center + QPointF(radius, -radius));
		painter->setPen(QPen(Qt::white, factor, Qt::SolidLine, Qt::SquareCap, Qt::RoundJoin));
		painter->drawLine(center + QPointF(-radius, -radius), center + QPointF(radius, radius));
		painter->drawLine(center + QPointF(-radius, radius), center + QPointF(radius, -radius));
		break;
	}
	case NormalHandle:
		draw_classic_ring(square, 3.5 * factor);
		break;
	case CurveHandle:
		draw_classic_ring(circle, 3.5 * factor);
		break;
	case DashHandle:
		draw_classic_ring(diamond, 4.5 * factor);
		break;
	}
	painter->restore();
}


void PointHandles::drawCurveHandleLine(render::OverlaySceneBuilder* painter, QPointF anchor_point, QPointF curve_handle, PointHandleType type, PointHandleState state) const
{
	const auto handle_radius = 3 * scale_factor;
	if (scale_factor > 1)
		painter->setPen(QPen(QBrush(stateColor(state)), scale_factor));
	else
		painter->setPen(stateColor(state));
	
	QPointF to_handle = curve_handle - anchor_point;
	auto to_handle_len = to_handle.x()*to_handle.x() + to_handle.y()*to_handle.y();
	if (to_handle_len > 0.00001)
	{
		to_handle_len = std::sqrt(to_handle_len);
		auto factor = qreal(scale_factor);
		switch (type)
		{
		case StartHandle:
			factor *= 5 / qMax(qAbs(to_handle.x()), qAbs(to_handle.y()));
			break;
		case DashHandle:
			factor *= 3 / to_handle_len;
			break;
		default:
			factor *= 3 / qMax(qAbs(to_handle.x()), qAbs(to_handle.y()));
		}
		anchor_point += to_handle * factor;
		curve_handle -= to_handle * (handle_radius / to_handle_len);
	}
	
	painter->drawLine(anchor_point, curve_handle);
}

}  // namespace OpenOrienteering
