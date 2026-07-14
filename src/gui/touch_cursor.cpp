/*
 *    Copyright 2013 Thomas Schöps
 *    Copyright 2015 Kai Pastor
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


#include "touch_cursor.h"

#include <Qt>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QRectF>

#include "gui/map/map_widget.h"
#include "gui/util_gui.h"


namespace OpenOrienteering {

// TODO: convert these to settings
const float touch_pos_offset_mm = 25;
const float control_ring_radius_mm = 9.5f;

TouchCursor::TouchCursor(MapWidget* map_widget)
: visible(false)
, left_button_pressed(false)
, last_pressed_button(NoButton)
, map_widget(map_widget)
{
	// nothing
}

TouchCursor::MouseEventTranslation TouchCursor::mousePressEvent(const QMouseEvent& event)
{
	if (event.button() != Qt::LeftButton)
		return {};
	last_touch_pos = event.position();
	first_move_event_received = false;
	
	ControlID control_id = NoButton;
	if (!visible || !touchedControl(event.position(), &control_id))
	{
		// Jump to position
		updateMapWidget(false);
		
		QPointF cursor_pos = event.position() - QPointF(0, touchPosOffsetPx());
		last_cursor_pos = cursor_pos;
		cursor_coord = map_widget->viewportToMapF(cursor_pos);
		visible = true;
		
		updateMapWidget(false);
		
		last_pressed_button = NoButton;
		return { MouseEventTranslation::Action::Replace, QEvent::MouseMove, cursor_pos,
		         Qt::NoButton, event.buttons() & ~Qt::LeftButton };
	}
	else if (control_id == LeftButton)
	{
		left_button_pressed = true;
		last_pressed_button = LeftButton;
		
		last_cursor_pos = map_widget->mapToViewport(cursor_coord);
		return { MouseEventTranslation::Action::Replace, QEvent::MouseButtonPress, last_cursor_pos,
		         event.button(), event.buttons() };
	}
	return { .action = MouseEventTranslation::Action::Discard };
}

TouchCursor::MouseEventTranslation TouchCursor::mouseMoveEvent(const QMouseEvent& event)
{
	if (!(event.buttons() & Qt::LeftButton))
		return { .action = MouseEventTranslation::Action::Discard };

	if (last_pressed_button != NoButton && !first_move_event_received)
	{
		first_move_event_received = true;
		last_touch_pos = event.position();
		return { .action = MouseEventTranslation::Action::Discard };
	}
	
	updateMapWidget(true);
	
	QPointF cursor_pos;
	if (last_pressed_button == LeftButton)
		cursor_pos = last_cursor_pos + (event.position() - last_touch_pos);
	else
		cursor_pos = event.position() - QPointF(0, touchPosOffsetPx());
	last_touch_pos = event.position();
	last_cursor_pos = cursor_pos;
	cursor_coord = map_widget->viewportToMapF(cursor_pos);
		
	updateMapWidget(true);
	return { MouseEventTranslation::Action::Replace, QEvent::MouseMove, cursor_pos,
	         left_button_pressed ? event.button() : Qt::NoButton,
	         left_button_pressed ? event.buttons() : (event.buttons() & ~Qt::LeftButton) };
}

TouchCursor::MouseEventTranslation TouchCursor::mouseReleaseEvent(const QMouseEvent& event)
{
	if (event.button() != Qt::LeftButton)
		return {};
	
	if (left_button_pressed)
	{
		left_button_pressed = false;
		return { MouseEventTranslation::Action::Replace, QEvent::MouseButtonRelease,
		         map_widget->mapToViewport(cursor_coord), event.button(), event.buttons() };
	}
	return { .action = MouseEventTranslation::Action::Discard };
}

TouchCursor::MouseEventTranslation TouchCursor::mouseDoubleClickEvent(const QMouseEvent& event)
{
	if (last_pressed_button == LeftButton)
	{
		return { MouseEventTranslation::Action::Replace, QEvent::MouseButtonDblClick,
		         map_widget->mapToViewport(cursor_coord), event.button(), event.buttons() };
	}
	return { .action = MouseEventTranslation::Action::Discard };
}

void TouchCursor::paint(render::OverlaySceneBuilder* painter)
{
	if (!visible)
		return;
	
	QPointF cursor_pos = map_widget->mapToViewport(cursor_coord);
	
	// Draw cursor
	QPixmap cursor_pixmap = map_widget->cursor().pixmap();
	if (!cursor_pixmap.isNull())
		painter->drawPixmap(cursor_pos - map_widget->cursor().hotSpot(), cursor_pixmap);
	else
	{
		// TODO: better standard "cursor"?
		float cursor_radius = standardCursorRadiusPx();
		
		painter->setPen(QPen(Qt::gray, controlRingStrokeRadiusPx()));
		painter->setBrush(Qt::NoBrush);
		painter->drawLine(cursor_pos - QPointF(cursor_radius, 0), cursor_pos + QPointF(cursor_radius, 0));
		painter->drawLine(cursor_pos - QPointF(0, cursor_radius), cursor_pos + QPointF(0, cursor_radius));
	}
	
	// Draw move handle / left button
	painter->setPen(QPen(Qt::gray, controlRingStrokeRadiusPx()));
	painter->setBrush(Qt::NoBrush);
	painter->drawEllipse(cursor_pos + QPointF(0, touchPosOffsetPx()), controlRingRadiusPx(), controlRingRadiusPx());
}

bool TouchCursor::touchedControl(const QPointF& pos, TouchCursor::ControlID* out_id)
{
	QPointF cursor_pos = map_widget->mapToViewport(cursor_coord);
	QPointF control_ring_center = cursor_pos + QPointF(0, touchPosOffsetPx());
	
	QPointF dist_to_center = pos - control_ring_center;
	if (dist_to_center.x()*dist_to_center.x() + dist_to_center.y()*dist_to_center.y() < controlRingRadiusPx()*controlRingRadiusPx())
	{
		*out_id = LeftButton;
		return true;
	}
	
	return false;
}

void TouchCursor::updateMapWidget(bool delayed)
{
	QRectF fake_rect = QRectF(cursor_coord.x(), cursor_coord.y(), 0.0001f, 0.0001f);
	float pixel_border = qMax(touchPosOffsetPx() + controlRingRadiusPx(), standardCursorRadiusPx()) + controlRingStrokeRadiusPx() + 1;
	
	if (delayed)
		map_widget->updateDrawingLater(fake_rect, pixel_border);
	else
		map_widget->updateDrawing(fake_rect, pixel_border);
}

float TouchCursor::touchPosOffsetPx() const
{
	return Util::mmToPixelLogical(touch_pos_offset_mm);
}

float TouchCursor::controlRingRadiusPx() const
{
	return Util::mmToPixelLogical(control_ring_radius_mm);
}

float TouchCursor::controlRingStrokeRadiusPx() const
{
	return Util::mmToPixelLogical(0.5f);
}

float TouchCursor::standardCursorRadiusPx() const
{
	return Util::mmToPixelLogical(1.5f);
}


}  // namespace OpenOrienteering
