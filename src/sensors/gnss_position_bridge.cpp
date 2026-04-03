/*
 *    Copyright 2013 Thomas Schöps
 *    Copyright 2014-2020 Kai Pastor
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

#include "gnss_position_bridge.h"

#include <cmath>

#if defined(Q_OS_ANDROID)
#  include <jni.h>
#  include <QJniObject>
#endif
#if defined(QT_POSITIONING_LIB)
#  include <QGeoCoordinate>
#  include <QGeoPositionInfo>
#  include <QGeoPositionInfoSource>  // IWYU pragma: keep
#endif

#include <QtGlobal>
#include <QDebug>
#include <QLatin1String>

#include "settings.h"
#include "core/georeferencing.h"
#include "core/latlon.h"
#include "gnss/gnss_solution.h"
#include "gnss/gnss_state.h"
#include "gnss/gnss_session.h"

#if defined(MAPPER_USE_FAKE_POSITION_PLUGIN)
#include "sensors/fake_position_source.h"
#endif

namespace OpenOrienteering {

GnssPositionBridge::GnssPositionBridge(const Georeferencing& georeferencing, QObject* parent)
 : QObject(parent)
 , georeferencing(georeferencing)
{
#if defined(QT_POSITIONING_LIB)
#if QT_VERSION < QT_VERSION_CHECK(5, 11, 0)
	static const int register_metatype = qRegisterMetaType<QGeoPositionInfo>();  // QTBUG-65937
	Q_UNUSED(register_metatype)
#endif

#if defined(MAPPER_USE_FAKE_POSITION_PLUGIN)
	{
		auto const ref = georeferencing.getGeographicRefPoint();
		FakePositionSource::setReferencePoint({ref.latitude(), ref.longitude(), 400});
	}
#endif

	auto const& settings = Settings::getInstance();
	auto const nmea_serialport = settings.nmeaSerialPort();
	if (!nmea_serialport.isEmpty())
		qputenv("QT_NMEA_SERIAL_PORT", nmea_serialport.toUtf8());

	auto source_name = settings.positionSource();
	if (source_name.isEmpty())
	{
		source_name = QLatin1String("default");
		source = QGeoPositionInfoSource::createDefaultSource(this);
	}
	else
	{
		source = QGeoPositionInfoSource::createSource(source_name, this);
	}

	if (!source)
	{
		qDebug("Cannot create QGeoPositionInfoSource '%s'!", qPrintable(source_name));
		return;
	}

	source->setPreferredPositioningMethods(QGeoPositionInfoSource::SatellitePositioningMethods);
	source->setUpdateInterval(1000);
	connect(source, &QGeoPositionInfoSource::positionUpdated, this, &GnssPositionBridge::positionUpdated, Qt::QueuedConnection);
	connect(source, &QGeoPositionInfoSource::errorOccurred, this, &GnssPositionBridge::errorOccurred);
#endif
}

GnssPositionBridge::~GnssPositionBridge()
{
	stopUpdates();
}


bool GnssPositionBridge::checkLocationServicesEnabled()
{
#if defined(Q_OS_ANDROID)
	static bool translation_initialized = false;
	if (!translation_initialized)
	{
		QJniObject location_disabled_string = QJniObject::fromString(tr("Location services are disabled in the device settings. Open settings now?"));
		QJniObject yes_string = QJniObject::fromString(tr("Yes"));
		QJniObject no_string  = QJniObject::fromString(tr("No"));
		QJniObject::callStaticMethod<void>(
			"org/openorienteering/mapper/MapperActivity",
			"setTranslatableStrings",
			"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
			yes_string.object<jstring>(),
			no_string.object<jstring>(),
			location_disabled_string.object<jstring>());
		translation_initialized = true;
	}

	QJniObject::callStaticMethod<void>("org/openorienteering/mapper/MapperActivity",
	                                   "checkGPSEnabled",
	                                   "()V");
#endif
	return true;
}

void GnssPositionBridge::setGnssSession(GnssSession* session)
{
	if (gnss_session == session)
		return;

	if (gnss_session)
		disconnect(gnss_session, nullptr, this, nullptr);

	gnss_session = session;
	resetPositionState();

	if (gnss_session)
	{
		connect(gnss_session, &GnssSession::solutionUpdated,
		        this, &GnssPositionBridge::gnssSolutionUpdated);
		connect(gnss_session, &GnssSession::stateChanged,
		        this, &GnssPositionBridge::gnssStateChanged);

		const auto& current_solution = gnss_session->currentSolution();
		if (current_solution.hasFreshPosition)
			gnssSolutionUpdated(current_solution);
		else
			gnssStateChanged(gnss_session->currentState());
	}
	else
	{
		emit displayStateChanged();
	}
}


void GnssPositionBridge::startUpdates()
{
	if (gnss_session)
	{
		// External GNSS sessions manage their own lifecycle.
		return;
	}
#if defined(QT_POSITIONING_LIB)
	if (source)
	{
		checkLocationServicesEnabled();
		source->startUpdates();
	}
#endif
}

void GnssPositionBridge::stopUpdates()
{
	if (gnss_session)
	{
		resetPositionState();
		emit displayStateChanged();
		return;
	}
#if defined(QT_POSITIONING_LIB)
	if (source)
	{
		source->stopUpdates();
		resetPositionState();
		emit displayStateChanged();
	}
#endif
}


void GnssPositionBridge::positionUpdated(const QGeoPositionInfo& info)
{
#if defined(QT_POSITIONING_LIB)
	const auto coord = info.coordinate();
	if (!coord.isValid())
		return;

	bool ok = false;
	latest_map_coord = georeferencing.toMapCoordF(LatLon(coord.latitude(), coord.longitude()), &ok);
	if (!ok)
		return;

	tracking_lost = false;
	has_live_position = true;
	has_display_position = true;
	gnss_fix_type = 0;
	gnss_ellipse_available = false;
	gnss_ellipse_semi_major = -1.0f;
	gnss_ellipse_semi_minor = -1.0f;
	gnss_ellipse_orientation = 0.0f;
	latest_horizontal_accuracy = info.hasAttribute(QGeoPositionInfo::HorizontalAccuracy)
	                            ? float(info.attribute(QGeoPositionInfo::HorizontalAccuracy))
	                            : -1.0f;
	latest_latitude = coord.latitude();
	latest_longitude = coord.longitude();
	latest_altitude = (coord.type() == QGeoCoordinate::Coordinate3D)
	    ? coord.altitude()
	    : std::numeric_limits<double>::quiet_NaN();

	emit mapPositionUpdated(latest_map_coord, latest_horizontal_accuracy);
	emit latLonUpdated(
	    latest_latitude,
	    latest_longitude,
	    std::isnan(latest_altitude) ? -9999.0 : latest_altitude,
	    latest_horizontal_accuracy
	);
	emit displayStateChanged();
#endif
}

void GnssPositionBridge::applyGnssSolution(const GnssSolutionSnapshot& solution, bool emit_signals)
{
	const auto& position = solution.position;
	if (!position.valid)
		return;

	bool ok = false;
	latest_map_coord = georeferencing.toMapCoordF(LatLon(position.latitude, position.longitude), &ok);
	if (!ok)
	{
		qWarning("GnssPositionBridge: GNSS coord conversion failed for %.6f, %.6f",
		         position.latitude, position.longitude);
		return;
	}

	tracking_lost = !solution.hasFreshPosition;
	has_live_position = solution.hasFreshPosition;
	has_display_position = true;

	gnss_fix_type = static_cast<std::uint8_t>(position.fixType);
	latest_latitude = position.latitude;
	latest_longitude = position.longitude;
	latest_altitude = std::isnan(position.altitudeMsl)
	    ? std::numeric_limits<double>::quiet_NaN()
	    : position.altitudeMsl;

	gnss_ellipse_available = position.ellipseAvailable;
	if (position.ellipseAvailable)
	{
		gnss_ellipse_semi_major = position.ellipseSemiMajorP95;
		gnss_ellipse_semi_minor = position.ellipseSemiMinorP95;
		gnss_ellipse_orientation = position.ellipseOrientationDeg;
	}
	else
	{
		gnss_ellipse_semi_major = -1.0f;
		gnss_ellipse_semi_minor = -1.0f;
		gnss_ellipse_orientation = 0.0f;
	}

	latest_horizontal_accuracy = !std::isnan(position.hAccuracyP95)
	    ? position.hAccuracyP95
	    : (!std::isnan(position.hAccuracy) ? position.hAccuracy : -1.0f);

	if (emit_signals)
	{
		emit mapPositionUpdated(latest_map_coord, latest_horizontal_accuracy);
		emit latLonUpdated(
		    latest_latitude,
		    latest_longitude,
		    std::isnan(latest_altitude) ? -9999.0 : latest_altitude,
		    latest_horizontal_accuracy
		);
	}
}

void GnssPositionBridge::refreshFromGnssSession()
{
	if (!gnss_session)
		return;

	const auto& solution = gnss_session->currentSolution();
	if (solution.position.valid)
	{
		applyGnssSolution(solution, false);
		return;
	}

	has_live_position = false;
	if (has_display_position)
		tracking_lost = true;
	gnss_fix_type = 0;
}

void GnssPositionBridge::gnssSolutionUpdated(const GnssSolutionSnapshot& solution)
{
	applyGnssSolution(solution, true);
	emit displayStateChanged();
}


void GnssPositionBridge::gnssStateChanged(const GnssState& state)
{
	Q_UNUSED(state);
	const bool had_live_position = has_live_position;
	refreshFromGnssSession();
	if (had_live_position && !has_live_position)
		emit positionUpdatesInterrupted();
	emit displayStateChanged();
}


#if defined(QT_POSITIONING_LIB)
void GnssPositionBridge::errorOccurred(QGeoPositionInfoSource::Error positioningError)
{
	Q_UNUSED(positioningError)
	if (source->error() != QGeoPositionInfoSource::NoError)
	{
		const bool had_live_position = has_live_position;
		has_live_position = false;
		if (has_display_position)
			tracking_lost = true;
		if (had_live_position)
			emit positionUpdatesInterrupted();
		emit displayStateChanged();
	}
}
#endif

void GnssPositionBridge::resetPositionState(bool clear_display_position)
{
	has_live_position = false;
	tracking_lost = false;
	if (clear_display_position)
	{
		has_display_position = false;
		latest_map_coord = MapCoordF();
		latest_latitude = std::numeric_limits<double>::quiet_NaN();
		latest_longitude = std::numeric_limits<double>::quiet_NaN();
		latest_altitude = std::numeric_limits<double>::quiet_NaN();
	}
	latest_horizontal_accuracy = -1.0f;
	gnss_fix_type = 0;
	gnss_ellipse_available = false;
	gnss_ellipse_semi_major = -1.0f;
	gnss_ellipse_semi_minor = -1.0f;
	gnss_ellipse_orientation = 0.0f;
}


}  // namespace OpenOrienteering
