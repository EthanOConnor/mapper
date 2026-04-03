/*
 *    Copyright 2013 Thomas Schöps
 *    Copyright 2016, 2018 Kai Pastor
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


#ifndef OPENORIENTEERING_GNSS_POSITION_BRIDGE_H
#define OPENORIENTEERING_GNSS_POSITION_BRIDGE_H

#include <cstdint>
#include <limits>

#include <QtGlobal>
#include <QObject>
#include <QString>

#if defined(QT_POSITIONING_LIB)
#  include <QGeoPositionInfoSource>
#endif

#include "core/map_coord.h"

class QGeoPositionInfo;
class QGeoPositionInfoSource;

namespace OpenOrienteering {

class Georeferencing;
class GnssSession;
struct GnssSolutionSnapshot;
struct GnssState;


/**
 * Tracks the current live position and exposes it to tools, tracking, and UI.
 *
 * This class bridges the current GNSS session / platform positioning inputs to
 * the existing tools, tracking, and follow-position consumers. Rendering is
 * handled by GnssMapOverlay.
 *
 * \todo Use qreal instead of float (in all sensor code) for consistency with Qt.
 */
class GnssPositionBridge : public QObject
{
Q_OBJECT
public:
	explicit GnssPositionBridge(const Georeferencing& georeferencing, QObject* parent = nullptr);
	~GnssPositionBridge() override;
	
	/**
	 * Checks if location services are enabled, and may guide the user to the device settings.
	 *
	 * If location services are not enabled in the device settings, it asks the user whether he
	 * wishes to open the device's location settings dialog.
	 * (At the moment, this is implemented for Android only.)
	 *
	 * Returns true if location services are enabled, but also when the settings dialog remains
	 * open when returning from this function and the final setting is unknown.
	 */
	bool checkLocationServicesEnabled();
	
	/// Starts regular position updates from the platform source.
	void startUpdates();
	/// Stops regular position updates and clears the current position state.
	void stopUpdates();
	
	/// Returns if a fresh live position was received since the last startUpdates().
	bool hasLivePosition() const { return has_live_position; }
	/// Returns true if there is a position to display, including a stale last fix.
	bool hasDisplayPosition() const { return has_display_position; }
	/// Returns true if the last displayed position is stale.
	bool isTrackingLost() const { return tracking_lost; }
	/// Returns the latest received map coordinate. Check hasDisplayPosition() beforehand.
	const MapCoordF& latestMapCoord() const { return latest_map_coord; }
	/// Returns the accuracy of the latest received position, or -1 if unknown.
	float latestHorizontalAccuracy() const { return latest_horizontal_accuracy; }
	
	/// Sets a GnssSession as the position source, replacing QGeoPositionInfoSource.
	/// Pass nullptr to revert to the default Qt positioning source.
	void setGnssSession(GnssSession* session);

	/// Returns the current GNSS fix type (0 = unknown/not using GNSS).
	std::uint8_t currentFixType() const { return gnss_fix_type; }
	bool hasUncertaintyEllipse() const { return gnss_ellipse_available; }
	float ellipseSemiMajorP95() const { return gnss_ellipse_semi_major; }
	float ellipseSemiMinorP95() const { return gnss_ellipse_semi_minor; }
	float ellipseOrientationDeg() const { return gnss_ellipse_orientation; }
	double latitude() const { return latest_latitude; }
	double longitude() const { return latest_longitude; }
	
signals:
	/// Is emitted whenever a new position update happens.
	/// If the accuracy is unknown, -1 will be given.
	void mapPositionUpdated(const OpenOrienteering::MapCoordF& coord, float accuracy);
	
	/// Like mapPositionUpdated(), but gives the values as
	/// latitude / longitude in degrees and also gives altitude
	/// (meters above sea level; -9999 is unknown)
	void latLonUpdated(double latitude, double longitude, double altitude, float accuracy);
	
	/// Is emitted when updates are interrupted after previously being active,
	/// due to loss of satellite reception or another error such as the user
	/// turning off location services or the receiver.
	void positionUpdatesInterrupted();
	
	/// Emitted whenever the displayable live-position state changes.
	void displayStateChanged();
	
private slots:
	void positionUpdated(const QGeoPositionInfo& info);
	void gnssSolutionUpdated(const OpenOrienteering::GnssSolutionSnapshot& solution);
	void gnssStateChanged(const OpenOrienteering::GnssState& state);
#if defined(QT_POSITIONING_LIB)
	void errorOccurred(QGeoPositionInfoSource::Error positioningError);
#endif
	
private:
	void applyGnssSolution(const OpenOrienteering::GnssSolutionSnapshot& solution, bool emit_signals);
	void refreshFromGnssSession();
	void resetPositionState(bool clear_display_position = true);
	
	const Georeferencing& georeferencing;
	QGeoPositionInfoSource* source = nullptr;
	GnssSession* gnss_session = nullptr;
	MapCoordF latest_map_coord;
	float latest_horizontal_accuracy = -1.0f;
	bool tracking_lost = false;
	bool has_live_position = false;
	bool has_display_position = false;

	std::uint8_t gnss_fix_type = 0;
	float gnss_ellipse_semi_major = -1.0f;
	float gnss_ellipse_semi_minor = -1.0f;
	float gnss_ellipse_orientation = 0.0f;
	bool gnss_ellipse_available = false;

	double latest_latitude = std::numeric_limits<double>::quiet_NaN();
	double latest_longitude = std::numeric_limits<double>::quiet_NaN();
	double latest_altitude = std::numeric_limits<double>::quiet_NaN();
};


}  // namespace OpenOrienteering

#endif
