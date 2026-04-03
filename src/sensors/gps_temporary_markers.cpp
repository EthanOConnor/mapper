/*
 *    Copyright 2014 Thomas Schöps
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


#include "gps_temporary_markers.h"

#include <Qt>
#include <QtGlobal>
#include <QBrush>
#include <QPainter>
#include <QPen>

#include "core/map_coord.h"
#include "core/map_view.h"
#include "gui/map/map_widget.h"
#include "sensors/gps_display.h"

class QPointF;


namespace OpenOrienteering {

GPSTemporaryMarkers::GPSTemporaryMarkers(MapWidget* widget, GPSDisplay* gps_display): QObject()
{
	this->widget = widget;
	this->gps_display = gps_display;
	recording_path = false;
	
	connect(gps_display, &GPSDisplay::mapPositionUpdated, this, &GPSTemporaryMarkers::newGPSPosition);
	
	widget->setTemporaryMarkerDisplay(this);
}

GPSTemporaryMarkers::~GPSTemporaryMarkers()
{
	widget->setTemporaryMarkerDisplay(nullptr);
}

bool GPSTemporaryMarkers::addPoint()
{
	if (!gps_display->hasValidPosition())
		return false;
	
	points.push_back(gps_display->getLatestGPSCoord());
	updateMapWidget();
	return true;
}

void GPSTemporaryMarkers::startPath()
{
	paths.push_back(std::vector< QPointF >());
	recording_path = true;
	if (gps_display->hasValidPosition())
		newGPSPosition(gps_display->getLatestGPSCoord(), gps_display->getLatestGPSCoordAccuracy());
}

void GPSTemporaryMarkers::stopPath()
{
	recording_path = false;
}

void GPSTemporaryMarkers::clear()
{
	points.clear();
	paths.clear();
	updateMapWidget();
}

void GPSTemporaryMarkers::paint(QPainter* painter)
{
	painter->save();
	widget->applyMapTransform(painter);
	painter->setClipping(false);
	painter->setRenderHint(QPainter::Antialiasing, true);

	const auto pixel_to_map = [this](qreal pixels) {
		return widget->getMapView()->pixelToLength(pixels);
	};
	const auto point_outer_radius = pixel_to_map(4.5);
	const auto point_inner_radius = pixel_to_map(3.25);
	const auto path_vertex_outer_radius = pixel_to_map(2.8);
	const auto path_vertex_inner_radius = pixel_to_map(1.8);
	auto outer_pen = QPen(QColor(Qt::white), 3.0);
	outer_pen.setCosmetic(true);
	outer_pen.setCapStyle(Qt::RoundCap);
	outer_pen.setJoinStyle(Qt::RoundJoin);
	auto inner_pen = QPen(QColor(0x22, 0x7C, 0xE8), 1.75);
	inner_pen.setCosmetic(true);
	inner_pen.setCapStyle(Qt::RoundCap);
	inner_pen.setJoinStyle(Qt::RoundJoin);

	// Draw paths with a framed stroke so they stay legible over imagery.
	painter->setBrush(Qt::NoBrush);
	painter->setPen(outer_pen);
	for (const auto& path : paths)
	{
		if (path.empty())
			continue;
		painter->drawPolyline(path.data(), static_cast<int>(path.size()));
	}
	
	painter->setPen(inner_pen);
	for (const auto& path : paths)
	{
		if (path.empty())
			continue;
		painter->drawPolyline(path.data(), static_cast<int>(path.size()));
	}

	painter->setPen(Qt::NoPen);
	painter->setBrush(QBrush(Qt::white));
	for (const auto& path : paths)
	{
		for (const auto& point : path)
			painter->drawEllipse(point, path_vertex_outer_radius, path_vertex_outer_radius);
	}
	painter->setBrush(QBrush(QColor(0x22, 0x7C, 0xE8)));
	for (const auto& path : paths)
	{
		for (const auto& point : path)
			painter->drawEllipse(point, path_vertex_inner_radius, path_vertex_inner_radius);
	}
	
	// Draw points
	painter->setPen(Qt::NoPen);
	
	painter->setBrush(QBrush(Qt::white));
	for (const auto& point : points)
		painter->drawEllipse(point, point_outer_radius, point_outer_radius);
	
	painter->setBrush(QBrush(QColor(0x22, 0x7C, 0xE8)));
	for (const auto& point : points)
		painter->drawEllipse(point, point_inner_radius, point_inner_radius);
	
	painter->restore();
}

void GPSTemporaryMarkers::newGPSPosition(const MapCoordF& coord, float accuracy)
{
	Q_UNUSED(accuracy);

	if (recording_path && ! paths.empty())
	{
		std::vector< QPointF >& path_coords = paths.back();
		path_coords.push_back(coord);
		updateMapWidget();
	}
}

void GPSTemporaryMarkers::updateMapWidget()
{
	// NOTE: could limit the updated area here
	widget->update();
}


}  // namespace OpenOrienteering
