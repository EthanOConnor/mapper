/*
 *    Copyright 2026 Ethan O'Connor
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

#include "gnss_map_overlay.h"

#include <cmath>
#include <type_traits>

#include <Qt>
#include <QtGlobal>
#include <QtMath>
#include <QBrush>
#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QTransform>
#include <QTimerEvent>

#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map_view.h"
#include "gui/map/map_widget.h"
#include "sensors/compass.h"
#include "sensors/gnss_position_bridge.h"

namespace OpenOrienteering {

namespace {

static qreal opacity_curve[] = { 0.8, 1.0, 0.8, 0.5, 0.2, 0.0, 0.2, 0.5 };

QPen cosmeticPen(const QColor& color, qreal width, Qt::PenStyle style = Qt::SolidLine)
{
	QPen pen(color, width);
	pen.setCosmetic(true);
	pen.setStyle(style);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	return pen;
}

bool isFinitePoint(const QPointF& point)
{
	return qIsFinite(point.x()) && qIsFinite(point.y());
}

QColor markerColor(bool tracking_lost, std::uint8_t fix_type)
{
	if (tracking_lost)
		return QColor(Qt::gray);
	if (fix_type >= 5)
		return QColor(0x22, 0x7C, 0xE8);
	if (fix_type == 4)
		return QColor(0xED, 0x9A, 0x14);
	if (fix_type == 3)
		return QColor(0x00, 0x96, 0x88);
	return QColor(Qt::red);
}

struct ScreenEllipse
{
	QPointF center;
	QPointF major_axis;
	QPointF minor_axis;
	bool valid = false;
};

bool projectProjectedOffsetToViewport(
    const MapWidget* widget,
    const Georeferencing& georeferencing,
    const QPointF& center_projected,
    const QPointF& projected_offset,
    QPointF& out_axis)
{
	const auto plus_map = georeferencing.toMapCoordF(center_projected + projected_offset);
	const auto minus_map = georeferencing.toMapCoordF(center_projected - projected_offset);
	const auto plus_viewport = widget->mapToViewport(plus_map);
	const auto minus_viewport = widget->mapToViewport(minus_map);
	if (!isFinitePoint(plus_viewport) || !isFinitePoint(minus_viewport))
		return false;
	out_axis = 0.5 * (plus_viewport - minus_viewport);
	return isFinitePoint(out_axis);
}

qreal metersToMap(const Georeferencing& georeferencing, qreal meters)
{
	return meters * qreal(1000000.0) / georeferencing.getScaleDenominator();
}

QPointF fallbackAxis(
    const MapWidget* widget,
    const Georeferencing& georeferencing,
    qreal radius_m,
    qreal bearing_deg)
{
	const auto radius_px = widget->getMapView()->lengthToPixel(metersToMap(georeferencing, radius_m));
	const auto angle = qDegreesToRadians(bearing_deg);
	return QPointF(std::sin(angle) * radius_px, -std::cos(angle) * radius_px);
}

bool buildGroundEllipse(
    const MapWidget* widget,
    const Georeferencing& georeferencing,
    const MapCoordF& map_coord,
    double latitude,
    double longitude,
    qreal major_m,
    qreal minor_m,
    qreal bearing_deg,
    ScreenEllipse& ellipse)
{
	ellipse.center = widget->mapToViewport(map_coord);
	if (!isFinitePoint(ellipse.center) || major_m <= 0.0 || minor_m <= 0.0)
		return false;

	bool ok_center = false;
	const auto projected_center = georeferencing.toProjectedCoords(LatLon(latitude, longitude), &ok_center);
	if (!ok_center)
	{
		ellipse.major_axis = fallbackAxis(widget, georeferencing, major_m, bearing_deg);
		ellipse.minor_axis = fallbackAxis(widget, georeferencing, minor_m, bearing_deg + 90.0);
		ellipse.valid = isFinitePoint(ellipse.major_axis) && isFinitePoint(ellipse.minor_axis);
		return ellipse.valid;
	}

	const auto major_angle = qDegreesToRadians(bearing_deg);
	const QPointF major_offset { std::sin(major_angle) * major_m, std::cos(major_angle) * major_m };
	const auto minor_angle = major_angle + qDegreesToRadians(90.0);
	const QPointF minor_offset { std::sin(minor_angle) * minor_m, std::cos(minor_angle) * minor_m };

	if (!projectProjectedOffsetToViewport(widget, georeferencing, projected_center, major_offset, ellipse.major_axis))
		return false;
	if (!projectProjectedOffsetToViewport(widget, georeferencing, projected_center, minor_offset, ellipse.minor_axis))
		return false;

	ellipse.valid = isFinitePoint(ellipse.major_axis) && isFinitePoint(ellipse.minor_axis);
	return ellipse.valid;
}

bool buildGroundCircle(
    const MapWidget* widget,
    const Georeferencing& georeferencing,
    const MapCoordF& map_coord,
    double latitude,
    double longitude,
    qreal radius_m,
    ScreenEllipse& ellipse)
{
	ellipse.center = widget->mapToViewport(map_coord);
	if (!isFinitePoint(ellipse.center) || radius_m <= 0.0)
		return false;

	bool ok_center = false;
	const auto projected_center = georeferencing.toProjectedCoords(LatLon(latitude, longitude), &ok_center);
	if (!ok_center)
	{
		const auto radius_px = widget->getMapView()->lengthToPixel(metersToMap(georeferencing, radius_m));
		ellipse.major_axis = QPointF(radius_px, 0);
		ellipse.minor_axis = QPointF(0, radius_px);
		ellipse.valid = true;
		return true;
	}

	if (!projectProjectedOffsetToViewport(widget, georeferencing, projected_center, QPointF(radius_m, 0), ellipse.major_axis))
		return false;
	if (!projectProjectedOffsetToViewport(widget, georeferencing, projected_center, QPointF(0, radius_m), ellipse.minor_axis))
		return false;

	ellipse.valid = true;
	return true;
}

bool buildBearingVector(
    const MapWidget* widget,
    const Georeferencing& georeferencing,
    double latitude,
    double longitude,
    qreal bearing_deg,
    QPointF& out_vector)
{
	bool ok_center = false;
	const auto projected_center = georeferencing.toProjectedCoords(LatLon(latitude, longitude), &ok_center);
	if (!ok_center)
	{
		out_vector = fallbackAxis(widget, georeferencing, 1.0, bearing_deg);
		return isFinitePoint(out_vector);
	}

	const auto angle = qDegreesToRadians(bearing_deg);
	const QPointF offset { std::sin(angle), std::cos(angle) };
	return projectProjectedOffsetToViewport(widget, georeferencing, projected_center, offset, out_vector);
}

void drawEllipseOutline(QPainter* painter, const ScreenEllipse& ellipse, const QColor& framing, const QColor& foreground, bool dashed, int fill_alpha)
{
	if (!ellipse.valid)
		return;

	QPainterPath unit_circle;
	unit_circle.addEllipse(QPointF(0, 0), 1.0, 1.0);
	QTransform transform(
	    ellipse.major_axis.x(), ellipse.major_axis.y(),
	    ellipse.minor_axis.x(), ellipse.minor_axis.y(),
	    ellipse.center.x(), ellipse.center.y());
	const auto path = transform.map(unit_circle);

	auto fill = foreground;
	fill.setAlpha(fill_alpha);
	if (fill_alpha > 0)
		painter->setBrush(QBrush(fill));
	else
		painter->setBrush(Qt::NoBrush);
	painter->setPen(cosmeticPen(framing, 2.5));
	painter->drawPath(path);
	painter->setPen(cosmeticPen(foreground, 1.5, dashed ? Qt::DashLine : Qt::SolidLine));
	painter->drawPath(path);
}

void drawEllipseStroke(QPainter* painter, const ScreenEllipse& ellipse, const QPen& pen)
{
	if (!ellipse.valid)
		return;

	QPainterPath unit_circle;
	unit_circle.addEllipse(QPointF(0, 0), 1.0, 1.0);
	QTransform transform(
	    ellipse.major_axis.x(), ellipse.major_axis.y(),
	    ellipse.minor_axis.x(), ellipse.minor_axis.y(),
	    ellipse.center.x(), ellipse.center.y());
	const auto path = transform.map(unit_circle);

	painter->setBrush(Qt::NoBrush);
	painter->setPen(pen);
	painter->drawPath(path);
}

QPolygonF toViewportPolyline(const MapWidget* widget, const std::vector<QPointF>& points)
{
	QPolygonF polyline;
	polyline.reserve(static_cast<int>(points.size()));
	for (const auto& point : points)
		polyline.push_back(widget->mapToViewport(point));
	return polyline;
}

}  // namespace


void GnssMapOverlay::PulsatingOpacity::start(QObject& object)
{
	if (!timer_id)
		timer_id = object.startTimer(1000 / std::extent<decltype(opacity_curve)>::value);
}

void GnssMapOverlay::PulsatingOpacity::stop(QObject& object)
{
	if (timer_id)
	{
		object.killTimer(timer_id);
		*this = {};
	}
}

bool GnssMapOverlay::PulsatingOpacity::advance()
{
	if (isActive())
	{
		++index;
		if (index < std::extent<decltype(opacity_curve)>::value)
			return true;
		index = 0;
	}
	return false;
}

qreal GnssMapOverlay::PulsatingOpacity::current() const
{
	if (!isActive())
		return 1.0;
	return opacity_curve[index];
}


GnssMapOverlay::GnssMapOverlay(MapWidget* widget, GnssPositionBridge* position_bridge, const Georeferencing& georeferencing, QObject* parent)
 : QObject(parent)
 , widget(widget)
 , position_bridge(position_bridge)
 , georeferencing(georeferencing)
{
	connect(position_bridge, &GnssPositionBridge::displayStateChanged,
	        this, &GnssMapOverlay::onDisplayStateChanged);
	connect(position_bridge, &GnssPositionBridge::mapPositionUpdated,
	        this, &GnssMapOverlay::onPositionUpdated);

	widget->setGnssMapOverlay(this);
}

GnssMapOverlay::~GnssMapOverlay()
{
	stopBlinking();
	widget->setGnssMapOverlay(nullptr);
}

void GnssMapOverlay::setVisible(bool visible)
{
	if (this->visible == visible)
		return;

	this->visible = visible;
	updateMapWidget();
}

void GnssMapOverlay::enableDistanceRings(bool enable)
{
	if (distance_rings_enabled == enable)
		return;

	distance_rings_enabled = enable;
	updateMapWidget();
}

void GnssMapOverlay::enableHeadingIndicator(bool enable)
{
	if (heading_indicator_enabled == enable)
		return;

	heading_indicator_enabled = enable;
	updateMapWidget();
}

void GnssMapOverlay::startBlinking(int seconds)
{
	blink_count = qMax(1, seconds);
	pulsating_opacity.start(*this);
	updateMapWidget();
}

void GnssMapOverlay::stopBlinking()
{
	blink_count = 0;
	pulsating_opacity.stop(*this);
	updateMapWidget();
}

bool GnssMapOverlay::addPoint()
{
	if (!position_bridge->hasLivePosition())
		return false;

	points.push_back(position_bridge->latestMapCoord());
	updateMapWidget();
	return true;
}

void GnssMapOverlay::startPath()
{
	paths.push_back(std::vector<QPointF>());
	recording_path = true;
	if (position_bridge->hasLivePosition())
		onPositionUpdated(position_bridge->latestMapCoord(), position_bridge->latestHorizontalAccuracy());
}

void GnssMapOverlay::stopPath()
{
	recording_path = false;
}

void GnssMapOverlay::clear()
{
	points.clear();
	paths.clear();
	updateMapWidget();
}

void GnssMapOverlay::paint(QPainter* painter)
{
	painter->save();
	painter->setClipping(false);
	painter->setRenderHint(QPainter::Antialiasing, true);

	const auto point_outer_radius = 4.5;
	const auto point_inner_radius = 3.25;
	const auto path_vertex_outer_radius = 2.8;
	const auto path_vertex_inner_radius = 1.8;
	auto outer_pen = cosmeticPen(QColor(Qt::white), 3.0);
	auto inner_pen = cosmeticPen(QColor(0x22, 0x7C, 0xE8), 1.75);

	painter->setBrush(Qt::NoBrush);
	painter->setPen(outer_pen);
	for (const auto& path : paths)
	{
		if (path.empty())
			continue;
		painter->drawPolyline(toViewportPolyline(widget, path));
	}

	painter->setPen(inner_pen);
	for (const auto& path : paths)
	{
		if (path.empty())
			continue;
		painter->drawPolyline(toViewportPolyline(widget, path));
	}

	painter->setPen(Qt::NoPen);
	painter->setBrush(QBrush(Qt::white));
	for (const auto& path : paths)
	{
		for (const auto& point : path)
			painter->drawEllipse(widget->mapToViewport(point), path_vertex_outer_radius, path_vertex_outer_radius);
	}
	painter->setBrush(QBrush(QColor(0x22, 0x7C, 0xE8)));
	for (const auto& path : paths)
	{
		for (const auto& point : path)
			painter->drawEllipse(widget->mapToViewport(point), path_vertex_inner_radius, path_vertex_inner_radius);
	}

	painter->setBrush(QBrush(Qt::white));
	for (const auto& point : points)
		painter->drawEllipse(widget->mapToViewport(point), point_outer_radius, point_outer_radius);

	painter->setBrush(QBrush(QColor(0x22, 0x7C, 0xE8)));
	for (const auto& point : points)
		painter->drawEllipse(widget->mapToViewport(point), point_inner_radius, point_inner_radius);

	if (!visible || !position_bridge->hasDisplayPosition())
	{
		painter->restore();
		return;
	}

	const auto center = widget->mapToViewport(position_bridge->latestMapCoord());
	if (!isFinitePoint(center))
	{
		painter->restore();
		return;
	}

	const auto tracking_lost = position_bridge->isTrackingLost();
	const auto foreground = markerColor(tracking_lost, position_bridge->currentFixType());
	const auto framing = QColor(Qt::white);
	const auto live_opacity = pulsating_opacity.current();
	painter->setOpacity(live_opacity * painter->opacity());

	const auto latitude = position_bridge->latitude();
	const auto longitude = position_bridge->longitude();
	const auto has_geo = qIsFinite(latitude) && qIsFinite(longitude);

	if (distance_rings_enabled && has_geo)
	{
		for (int i = 1; i <= 2; ++i)
		{
			ScreenEllipse ring;
			if (buildGroundCircle(widget, georeferencing, position_bridge->latestMapCoord(), latitude, longitude, 10.0 * i, ring))
				drawEllipseStroke(painter, ring, cosmeticPen(QColor(0, 0, 0, 110), 1.0, Qt::DashLine));
		}
	}

	if (position_bridge->hasUncertaintyEllipse() && has_geo)
	{
		ScreenEllipse ellipse;
		if (buildGroundEllipse(widget,
		                       georeferencing,
		                       position_bridge->latestMapCoord(),
		                       latitude,
		                       longitude,
		                       position_bridge->ellipseSemiMajorP95(),
		                       position_bridge->ellipseSemiMinorP95(),
		                       position_bridge->ellipseOrientationDeg(),
		                       ellipse))
		{
			drawEllipseOutline(painter, ellipse, framing, foreground, tracking_lost, tracking_lost ? 16 : 36);
		}
	}
	else if (position_bridge->latestHorizontalAccuracy() >= 0.0f && has_geo)
	{
		ScreenEllipse circle;
		if (buildGroundCircle(widget,
		                      georeferencing,
		                      position_bridge->latestMapCoord(),
		                      latitude,
		                      longitude,
		                      position_bridge->latestHorizontalAccuracy(),
		                      circle))
		{
			drawEllipseOutline(painter, circle, framing, foreground, tracking_lost, tracking_lost ? 16 : 36);
		}
	}

	if (heading_indicator_enabled && has_geo)
	{
		QPointF direction;
		if (buildBearingVector(widget,
		                       georeferencing,
		                       latitude,
		                       longitude,
		                       Compass::getInstance().getCurrentAzimuth(),
		                       direction))
		{
			const auto length = std::hypot(direction.x(), direction.y());
			if (length > 0.001)
			{
				const QPointF unit = direction / length;
				const QPointF perp(-unit.y(), unit.x());
				const QPointF tip = center + (unit * 14.0);
				const QPointF left = center - (unit * 4.0) + (perp * 6.0);
				const QPointF right = center - (unit * 4.0) - (perp * 6.0);
				QPolygonF arrow;
				arrow << tip << left << center << right;

				painter->setPen(cosmeticPen(framing, 2.5));
				painter->setBrush(foreground);
				painter->drawPolygon(arrow);
				painter->setPen(cosmeticPen(QColor(0, 0, 0, 90), 1.5));
				painter->setBrush(Qt::NoBrush);
				painter->drawLine(center, center + (unit * 320.0));
			}
		}
	}
	else
	{
		const auto dot_outer_radius = 7.0;
		const auto dot_inner_radius = 4.75;
		const auto center_radius = 1.6;
		const auto cross_inner = 9.0;
		const auto cross_outer = 18.0;

		painter->setPen(Qt::NoPen);
		painter->setBrush(framing);
		painter->drawEllipse(center, dot_outer_radius, dot_outer_radius);
		painter->setBrush(foreground);
		painter->drawEllipse(center, dot_inner_radius, dot_inner_radius);
		painter->setBrush(framing);
		painter->drawEllipse(center, center_radius, center_radius);

		const auto draw_crosshairs = [painter, center, cross_inner, cross_outer]() {
			painter->drawLine(center - QPointF(cross_inner, 0), center - QPointF(cross_outer, 0));
			painter->drawLine(center + QPointF(cross_inner, 0), center + QPointF(cross_outer, 0));
			painter->drawLine(center - QPointF(0, cross_inner), center - QPointF(0, cross_outer));
			painter->drawLine(center + QPointF(0, cross_inner), center + QPointF(0, cross_outer));
		};
		painter->setBrush(Qt::NoBrush);
		painter->setPen(cosmeticPen(framing, 4.0));
		draw_crosshairs();
		painter->setPen(cosmeticPen(foreground, 2.0));
		draw_crosshairs();
	}

	painter->restore();
}

void GnssMapOverlay::timerEvent(QTimerEvent* e)
{
	if (e->timerId() == pulsating_opacity.timerId())
	{
		if (!pulsating_opacity.advance())
		{
			--blink_count;
			if (blink_count <= 0)
				stopBlinking();
		}
		updateMapWidget();
	}
}

void GnssMapOverlay::onDisplayStateChanged()
{
	updateMapWidget();
}

void GnssMapOverlay::onPositionUpdated(const MapCoordF& coord, float accuracy)
{
	Q_UNUSED(accuracy);

	if (recording_path && !paths.empty())
	{
		auto& path_coords = paths.back();
		path_coords.push_back(coord);
		updateMapWidget();
	}
}

void GnssMapOverlay::updateMapWidget()
{
	widget->update();
}


}  // namespace OpenOrienteering
