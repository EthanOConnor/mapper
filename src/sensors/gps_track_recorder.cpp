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


#include "gps_track_recorder.h"

#include <cmath>

#include <QDateTime>
#include <QtGlobal>

#include "core/latlon.h"
#include "gnss/gnss_position.h"
#include "core/map.h"
#include "core/map_view.h"
#include "core/track.h"
#include "gui/map/map_widget.h"
#include "sensors/gps_display.h"
#include "templates/template_track.h"


namespace OpenOrienteering {

GPSTrackRecorder::GPSTrackRecorder(GPSDisplay* gps_display, TemplateTrack* target_template, int draw_update_interval_milliseconds, MapWidget* widget)
 : QObject()
{
	this->gps_display = gps_display;
	this->target_template = target_template;
	this->widget = widget;
	
	track_changed_since_last_update = false;
	is_active = true;
	
	// Start with a new segment
	target_template->getTrack().finishCurrentSegment();
	
	if (gps_display)
	{
		// The GnssPosition signal carries the full fix (accuracy, fix type,
		// DOP, satellites, timestamp); latLonUpdated is its lossy collapse
		// and must not be connected in addition (double points).
		connect(gps_display, &GPSDisplay::gnssPositionUpdated, this, &GPSTrackRecorder::newGnssPosition);
		connect(gps_display, &GPSDisplay::positionUpdatesInterrupted, this, &GPSTrackRecorder::positionUpdatesInterrupted);
	}
	connect(target_template->getMap(), &Map::templateDeleted, this, &GPSTrackRecorder::templateDeleted);
	
	if (draw_update_interval_milliseconds > 0)
	{
		connect(&draw_update_timer, &QTimer::timeout, this, &GPSTrackRecorder::drawUpdate);
		draw_update_timer.start(draw_update_interval_milliseconds);
	}
	connect(&persist_timer, &QTimer::timeout,
	        this, &GPSTrackRecorder::persistUpdate);
	persist_timer.start(10 * 1000);
}

GPSTrackRecorder::~GPSTrackRecorder()
{
	persistUpdate();
}

namespace {

TrackFixType toTrackFixType(GnssFixType fix_type)
{
	switch (fix_type)
	{
	case GnssFixType::NoFix:
		// A recorded position with "NoFix" means the source did not report
		// a fix type, not that there was no fix.
		return TrackFixType::Unknown;
	case GnssFixType::Fix2D:
		return TrackFixType::Fix2D;
	case GnssFixType::Fix3D:
		return TrackFixType::Fix3D;
	case GnssFixType::DGPS:
		return TrackFixType::DGPS;
	case GnssFixType::RtkFloat:
		return TrackFixType::RtkFloat;
	case GnssFixType::RtkFixed:
		return TrackFixType::RtkFixed;
	}
	return TrackFixType::Unknown;
}

}  // namespace


void GPSTrackRecorder::newGnssPosition(const GnssPosition& position, const QString& accuracy_basis)
{
	auto new_point = TrackPoint { LatLon(position.latitude, position.longitude) };
	// Use the fix's own timestamp; reception time skews multi-Hz logs.
	new_point.datetime = position.timestamp.isValid()
	                     ? position.timestamp.toUTC()
	                     : QDateTime::currentDateTimeUtc();
	// GPX <ele> is height above mean sea level.
	if (std::isfinite(position.altitudeMsl))
		new_point.elevation = static_cast<float>(position.altitudeMsl);
	else if (std::isfinite(position.altitude))
		new_point.elevation = static_cast<float>(position.altitude);
	new_point.hDOP = position.hDOP;  // true DOP or NaN - never meters
	new_point.hAccuracy = position.hAccuracy;
	new_point.vAccuracy = position.vAccuracy;
	new_point.correctionAge = position.correctionAge;
	new_point.fixType = toTrackFixType(position.fixType);
	new_point.satellitesUsed = position.satellitesUsed > 0 ? int(position.satellitesUsed) : -1;
	new_point.accuracyBasis = accuracy_basis;
	target_template->getTrack().appendTrackPoint(new_point);
	target_template->setHasUnsavedChanges(true);
	track_changed_since_last_update = true;
}

void GPSTrackRecorder::newPosition(double latitude, double longitude, double altitude, float accuracy)
{
	auto new_point = TrackPoint { LatLon(latitude, longitude) };
	// No fix timestamp is available on this legacy path.
	new_point.datetime = QDateTime::currentDateTimeUtc();
	if (altitude > -9999)
		new_point.elevation = static_cast<float>(altitude);
	// The accuracy is horizontal accuracy in meters, not DOP: it must not
	// end up in TrackPoint::hDOP (and thus in GPX <hdop>).
	if (accuracy >= 0)
		new_point.hAccuracy = accuracy;
	target_template->getTrack().appendTrackPoint(new_point);
	target_template->setHasUnsavedChanges(true);
	track_changed_since_last_update = true;
}

void GPSTrackRecorder::positionUpdatesInterrupted()
{
	target_template->getTrack().finishCurrentSegment();
	target_template->setHasUnsavedChanges(true);
	track_changed_since_last_update = true;
	persistUpdate();
}

void GPSTrackRecorder::templateDeleted(int pos, const Template* old_temp)
{
	Q_UNUSED(pos);
	if (!is_active)
		return;
	
	if (old_temp == target_template)
	{
		// Deactivate
		persistUpdate();
		gps_display->disconnect(this);
		draw_update_timer.stop();
		persist_timer.stop();
		is_active = false;
	}
}

void GPSTrackRecorder::persistUpdate()
{
	if (!is_active || !target_template->hasUnsavedChanges())
		return;
	const auto path = target_template->getTemplatePath();
	if (path.isEmpty() || !target_template->writeTemplateFile(path))
	{
		qWarning("Could not persist live GPS track %s",
		         qUtf8Printable(path));
		return;
	}
	target_template->setHasUnsavedChanges(false);
}

void GPSTrackRecorder::drawUpdate()
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
