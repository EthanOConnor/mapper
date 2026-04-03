/*
 *    Copyright 2014 Thomas Schöps
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


#include "gnss_track_recorder.h"

#include <QDateTime>
#include <QtGlobal>

#include "core/latlon.h"
#include "core/map.h"
#include "core/map_view.h"
#include "core/track.h"
#include "gui/map/map_widget.h"
#include "sensors/gnss_position_bridge.h"
#include "templates/template_track.h"


namespace OpenOrienteering {

GnssTrackRecorder::GnssTrackRecorder(GnssPositionBridge* position_bridge, TemplateTrack* target_template, int draw_update_interval_milliseconds, MapWidget* widget)
 : QObject()
{
	this->position_bridge = position_bridge;
	this->target_template = target_template;
	this->widget = widget;
	
	track_changed_since_last_update = false;
	is_active = true;
	
	// Start with a new segment
	target_template->getTrack().finishCurrentSegment();
	
	connect(position_bridge, &GnssPositionBridge::latLonUpdated, this, &GnssTrackRecorder::newPosition);
	connect(position_bridge, &GnssPositionBridge::positionUpdatesInterrupted, this, &GnssTrackRecorder::positionUpdatesInterrupted);
	connect(target_template->getMap(), &Map::templateDeleted, this, &GnssTrackRecorder::templateDeleted);
	
	if (draw_update_interval_milliseconds > 0)
	{
		connect(&draw_update_timer, &QTimer::timeout, this, &GnssTrackRecorder::drawUpdate);
		draw_update_timer.start(draw_update_interval_milliseconds);
	}
}

void GnssTrackRecorder::newPosition(double latitude, double longitude, double altitude, float accuracy)
{
	auto new_point = TrackPoint {
		LatLon(latitude, longitude),
		QDateTime::currentDateTimeUtc(),
		static_cast<float>(altitude),
		accuracy,
	};
	// Record fix type from the live-position bridge if available.
	if (position_bridge)
		new_point.fixType = position_bridge->currentFixType();
	target_template->getTrack().appendTrackPoint(new_point);
	target_template->setHasUnsavedChanges(true);
	track_changed_since_last_update = true;
}

void GnssTrackRecorder::positionUpdatesInterrupted()
{
	target_template->getTrack().finishCurrentSegment();
	target_template->setHasUnsavedChanges(true);
	track_changed_since_last_update = true;
}

void GnssTrackRecorder::templateDeleted(int pos, const Template* old_temp)
{
	Q_UNUSED(pos);
	if (!is_active)
		return;
	
	if (old_temp == target_template)
	{
		// Deactivate
		position_bridge->disconnect(this);
		draw_update_timer.stop();
		is_active = false;
	}
}

void GnssTrackRecorder::drawUpdate()
{
	if (!is_active)
		return;
	
	if (track_changed_since_last_update)
	{
		if (widget->getMapView()->isTemplateVisible(target_template))
			target_template->setTemplateAreaDirty();
		
		track_changed_since_last_update = false;
	}
}


}  // namespace OpenOrienteering
