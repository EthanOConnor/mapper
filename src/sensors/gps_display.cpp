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

#include "gps_display.h"

#if defined(Q_OS_ANDROID)
#  include <jni.h>
#  include <QJniObject>
#endif
#if defined(QT_POSITIONING_LIB)
#  include <QGeoCoordinate>
#  include <QGeoPositionInfo>
#  include <QGeoPositionInfoSource>  // IWYU pragma: keep
#endif

#include <algorithm>
#include <type_traits>

#include <Qt>
#include <QtGlobal>
#include <QtMath>
#include <QColor>
#include <QFlags>
#include <QLatin1String>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QPointF>
#include <QTransform>
#include <QTimer>  // IWYU pragma: keep
#include <QTimerEvent>

#include "settings.h"
#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map_view.h"
#include "gui/util_gui.h"
#include "gui/map/map_widget.h"
#include "gnss/gnss_solution.h"
#include "gnss/gnss_state.h"
#include "sensors/compass.h"
#include "util/backports.h"  // IWYU pragma: keep

#include "gnss/gnss_session.h"

#if defined(MAPPER_USE_FAKE_POSITION_PLUGIN)
#include "sensors/fake_position_source.h"
#endif

namespace OpenOrienteering {

namespace {

// Opacities as understood by QPainter::setOpacity().
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

}  // namespace


void GPSDisplay::PulsatingOpacity::start(QObject& object)
{
	if (!timer_id)
		timer_id = object.startTimer(1000 / std::extent<decltype(opacity_curve)>::value);
}

void GPSDisplay::PulsatingOpacity::stop(QObject& object)
{
	if (timer_id)
	{
		object.killTimer(timer_id);
		*this = {};
	}
}

bool GPSDisplay::PulsatingOpacity::advance()
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

qreal GPSDisplay::PulsatingOpacity::current() const
{
	if (!isActive())
		return 1.0;
	return opacity_curve[index];
}



GPSDisplay::GPSDisplay(MapWidget* widget, const Georeferencing& georeferencing, QObject* parent)
 : QObject(parent)
 , widget(widget)
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
	
	auto const & settings = Settings::getInstance();
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
	connect(source, &QGeoPositionInfoSource::positionUpdated, this, &GPSDisplay::positionUpdated, Qt::QueuedConnection);
	connect(source, &QGeoPositionInfoSource::errorOccurred, this, &GPSDisplay::errorOccurred);
#endif

	widget->setGPSDisplay(this);
}

GPSDisplay::~GPSDisplay()
{
	stopUpdates();
	widget->setGPSDisplay(nullptr);
}

bool GPSDisplay::checkGPSEnabled()
{
#if defined(Q_OS_ANDROID)
	static bool translation_initialized = false;
	if (!translation_initialized)
	{
		QJniObject gps_disabled_string = QJniObject::fromString(tr("GPS is disabled in the device settings. Open settings now?"));
		QJniObject yes_string = QJniObject::fromString(tr("Yes"));
		QJniObject no_string  = QJniObject::fromString(tr("No"));
		QJniObject::callStaticMethod<void>(
			"org/openorienteering/mapper/MapperActivity",
			"setTranslatableStrings",
			"(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V",
			yes_string.object<jstring>(),
			no_string.object<jstring>(),
			gps_disabled_string.object<jstring>());
		translation_initialized = true;
	}
	
	QJniObject::callStaticMethod<void>("org/openorienteering/mapper/MapperActivity",
                                       "checkGPSEnabled",
                                       "()V");
#endif
	return true;
}

void GPSDisplay::setGnssSession(GnssSession* session)
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
		        this, &GPSDisplay::gnssSolutionUpdated);
		connect(gnss_session, &GnssSession::stateChanged,
		        this, &GPSDisplay::gnssStateChanged);

		const auto& current_solution = gnss_session->currentSolution();
		if (current_solution.hasFreshPosition)
			gnssSolutionUpdated(current_solution);
		else
			gnssStateChanged(gnss_session->currentState());
	}

	updateMapWidget();
}


void GPSDisplay::startUpdates()
{
	if (gnss_session)
	{
		// External GNSS sessions manage their own lifecycle.
		return;
	}
#if defined(QT_POSITIONING_LIB)
	if (source)
	{
		checkGPSEnabled();
		source->startUpdates();
	}
#endif
}

void GPSDisplay::stopUpdates()
{
	if (gnss_session)
	{
		resetPositionState();
		updateMapWidget();
		return;
	}
#if defined(QT_POSITIONING_LIB)
	if (source)
	{
		source->stopUpdates();
		resetPositionState();
		updateMapWidget();
	}
#endif
}

void GPSDisplay::setVisible(bool visible)
{
	if (this->visible != visible)
	{
		this->visible = visible;
		updateMapWidget();
	}
}

void GPSDisplay::enableDistanceRings(bool enable)
{
	distance_rings_enabled = enable;
	if (visible && has_display_position)
		updateMapWidget();
}

void GPSDisplay::enableHeadingIndicator(bool enable)
{
	if (enable && ! heading_indicator_enabled)
		Compass::getInstance().startUsage();
	else if (! enable && heading_indicator_enabled)
		Compass::getInstance().stopUsage();
	
	heading_indicator_enabled = enable;
}

void GPSDisplay::paint(QPainter* painter)
{
	if (gnss_session)
		refreshFromGnssSession();

	if (!visible || !has_display_position)
		return;

	const QPointF gps_pos = latest_gps_coord;
	const auto pixel_to_map = [this](qreal pixels) {
		return widget->getMapView()->pixelToLength(pixels);
	};
	const auto meters_to_map = qreal(1000000.0) / georeferencing.getScaleDenominator();
	const auto cross_inner = pixel_to_map(9.0);
	const auto cross_outer = pixel_to_map(18.0);
	const auto dot_outer_radius = pixel_to_map(7.0);
	const auto dot_inner_radius = pixel_to_map(4.75);
	const auto center_radius = pixel_to_map(1.6);

	painter->save();
	widget->applyMapTransform(painter);
	painter->setClipping(false);
	painter->setRenderHint(QPainter::Antialiasing, true);
	painter->setOpacity(pulsating_opacity.current() * painter->opacity());
	
	// Color by fix type when using GNSS session, otherwise original behavior.
	const auto framing = QColor(Qt::white);
	QColor foreground;
	if (tracking_lost)
		foreground = QColor(Qt::gray);
	else if (gnss_fix_type >= 5)   // RtkFixed
		foreground = QColor(0x22, 0x7C, 0xE8);  // blue
	else if (gnss_fix_type == 4)   // RtkFloat
		foreground = QColor(0xED, 0x9A, 0x14);  // amber
	else if (gnss_fix_type == 3)   // DGPS
		foreground = QColor(0x00, 0x96, 0x88);  // teal
	else if (gnss_fix_type >= 1)   // Fix2D or Fix3D
		foreground = QColor(Qt::red);
	else if (gnss_fix_type == 0 && gnss_session)
		foreground = QColor(Qt::gray);           // GNSS active but no fix
	else
		foreground = QColor(Qt::red);            // default (Qt positioning)
	
	// Draw center dot or arrow
	if (heading_indicator_enabled)
	{
		const auto heading_rotation_deg = qreal(Compass::getInstance().getCurrentAzimuth());
		
		painter->save();
		painter->translate(gps_pos);
		painter->rotate(heading_rotation_deg);
		
		// Draw arrow
		static const QPointF arrow_points[4] = {
		    { 0, -pixel_to_map(14.0) },
		    { pixel_to_map(7.0), pixel_to_map(7.0) },
		    { 0, 0 },
		    { -pixel_to_map(7.0), pixel_to_map(7.0) }
		};
		painter->setPen(cosmeticPen(framing, 2.5));
		painter->setBrush(foreground);
		painter->drawPolygon(arrow_points, std::extent<decltype(arrow_points)>::value);
		
		// Draw heading line
		painter->setPen(cosmeticPen(QColor(0, 0, 0, 90), 1.5));
		painter->setBrush(Qt::NoBrush);
		painter->drawLine(QPointF(0, 0), QPointF(0, -pixel_to_map(320.0)));
		
		painter->restore();
	}
	else
	{
		painter->setPen(Qt::NoPen);
		painter->setBrush(framing);
		painter->drawEllipse(gps_pos, dot_outer_radius, dot_outer_radius);
		painter->setBrush(foreground);
		painter->drawEllipse(gps_pos, dot_inner_radius, dot_inner_radius);
		painter->setBrush(framing);
		painter->drawEllipse(gps_pos, center_radius, center_radius);
		
		const auto draw_crosshairs = [painter, gps_pos, cross_inner, cross_outer]() {
			painter->drawLine(gps_pos - QPointF{cross_inner, 0}, gps_pos - QPointF{cross_outer, 0});
			painter->drawLine(gps_pos + QPointF{cross_inner, 0}, gps_pos + QPointF{cross_outer, 0});
			painter->drawLine(gps_pos - QPointF{0, cross_inner}, gps_pos - QPointF{0, cross_outer});
			painter->drawLine(gps_pos + QPointF{0, cross_inner}, gps_pos + QPointF{0, cross_outer});
		};
		painter->setBrush(Qt::NoBrush);
		painter->setPen(cosmeticPen(framing, 4.0));
		draw_crosshairs();
		painter->setPen(cosmeticPen(foreground, 2.0));
		draw_crosshairs();
	}
	
	// Draw distance circles
	if (distance_rings_enabled)
	{
		const auto num_distance_rings = 2;
		const auto distance_ring_radius_meters = 10;
		const auto distance_ring_radius_map = distance_ring_radius_meters * meters_to_map;
		painter->setPen(cosmeticPen(QColor(0, 0, 0, 110), 1.0, Qt::DashLine));
		painter->setBrush(Qt::NoBrush);
		auto radius = distance_ring_radius_map;
		for (int i = 0; i < num_distance_rings; ++i)
		{
			painter->drawEllipse(gps_pos, radius, radius);
			radius += distance_ring_radius_map;
		}
	}
	
	// Draw accuracy circle or P95 error ellipse
	if (gnss_ellipse_available)
	{
		// Draw P95 error ellipse from covariance matrix
		const auto major_map = qreal(gnss_ellipse_semi_major) * meters_to_map;
		const auto minor_map = qreal(gnss_ellipse_semi_minor) * meters_to_map;
		// Orientation is from North clockwise; add map rotation
		const auto rotation_deg = qreal(gnss_ellipse_orientation);

		painter->save();
		painter->translate(gps_pos);
		painter->rotate(rotation_deg);

		// Semi-transparent fill
		auto fill_color = foreground;
		fill_color.setAlpha(tracking_lost ? 16 : 36);
		painter->setBrush(fill_color);
		painter->setPen(cosmeticPen(framing, 2.5));
		painter->drawEllipse(QPointF(0, 0), major_map, minor_map);
		painter->setPen(cosmeticPen(foreground, 1.5, tracking_lost ? Qt::DashLine : Qt::SolidLine));
		painter->drawEllipse(QPointF(0, 0), major_map, minor_map);

		painter->restore();
	}
	else if (latest_gps_coord_accuracy >= 0)
	{
		const auto accuracy_map = qreal(latest_gps_coord_accuracy) * meters_to_map;

		// Semi-transparent fill for P95 circle when using GNSS
		if (gnss_fix_type > 0)
		{
			auto fill_color = foreground;
			fill_color.setAlpha(tracking_lost ? 16 : 36);
			painter->setBrush(fill_color);
		}
		else
		{
			painter->setBrush(Qt::NoBrush);
		}

		painter->setPen(cosmeticPen(framing, 2.5));
		painter->drawEllipse(gps_pos, accuracy_map, accuracy_map);
		painter->setPen(cosmeticPen(foreground, 1.5, tracking_lost ? Qt::DashLine : Qt::SolidLine));
		painter->drawEllipse(gps_pos, accuracy_map, accuracy_map);
	}
	
	painter->restore();
}


void GPSDisplay::startBlinking(int seconds)
{
	blink_count = std::max(1, seconds);
	pulsating_opacity.start(*this);
}

void GPSDisplay::stopBlinking()
{
	blink_count = 0;
	pulsating_opacity.stop(*this);
}

void GPSDisplay::timerEvent(QTimerEvent* e)
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


void GPSDisplay::positionUpdated(const QGeoPositionInfo& info)
{
#if defined(QT_POSITIONING_LIB)
	const auto coord = info.coordinate();
	if (!coord.isValid())
		return;

	bool ok = false;
	latest_gps_coord = georeferencing.toMapCoordF(LatLon(coord.latitude(), coord.longitude()), &ok);
	if (!ok)
		return;

	tracking_lost = false;
	has_valid_position = true;
	has_display_position = true;
	gnss_fix_type = 0;
	gnss_h_accuracy_p95 = -1;
	gnss_ellipse_available = false;
	latest_gps_coord_accuracy = info.hasAttribute(QGeoPositionInfo::HorizontalAccuracy)
	                            ? float(info.attribute(QGeoPositionInfo::HorizontalAccuracy))
	                            : -1.0f;

	emit mapPositionUpdated(latest_gps_coord, latest_gps_coord_accuracy);
	emit latLonUpdated(
	    coord.latitude(),
	    coord.longitude(),
	    (coord.type() == QGeoCoordinate::Coordinate3D) ? coord.altitude() : -9999,
	    latest_gps_coord_accuracy
	);

	updateMapWidget();
#endif
}

void GPSDisplay::applyGnssSolution(const GnssSolutionSnapshot& solution, bool emit_signals)
{
	const auto& position = solution.position;
	if (!position.valid)
		return;

	bool ok = false;
	latest_gps_coord = georeferencing.toMapCoordF(LatLon(position.latitude, position.longitude), &ok);
	if (!ok)
	{
		qWarning("GPSDisplay: GNSS coord conversion failed for %.6f, %.6f",
		         position.latitude, position.longitude);
		return;
	}

	tracking_lost = !solution.hasFreshPosition;
	has_valid_position = solution.hasFreshPosition;
	has_display_position = true;

	gnss_fix_type = static_cast<std::uint8_t>(position.fixType);
	gnss_h_accuracy_p95 = position.hAccuracyP95;
	gnss_latitude = position.latitude;
	gnss_longitude = position.longitude;
	gnss_altitude = std::isnan(position.altitudeMsl) ? -9999.0 : position.altitudeMsl;

	gnss_ellipse_available = position.ellipseAvailable;
	if (position.ellipseAvailable)
	{
		gnss_ellipse_semi_major = position.ellipseSemiMajorP95;
		gnss_ellipse_semi_minor = position.ellipseSemiMinorP95;
		gnss_ellipse_orientation = position.ellipseOrientationDeg;
	}
	else
	{
		gnss_ellipse_semi_major = -1;
		gnss_ellipse_semi_minor = -1;
		gnss_ellipse_orientation = 0;
	}

	latest_gps_coord_accuracy = !std::isnan(position.hAccuracyP95)
	    ? position.hAccuracyP95
	    : (!std::isnan(position.hAccuracy) ? position.hAccuracy : -1.0f);

	if (emit_signals)
	{
		emit mapPositionUpdated(latest_gps_coord, latest_gps_coord_accuracy);
		emit latLonUpdated(position.latitude, position.longitude, gnss_altitude, latest_gps_coord_accuracy);
	}
}

void GPSDisplay::refreshFromGnssSession()
{
	if (!gnss_session)
		return;

	const auto& solution = gnss_session->currentSolution();
	if (solution.position.valid)
	{
		applyGnssSolution(solution, false);
		return;
	}

	has_valid_position = false;
	if (has_display_position)
		tracking_lost = true;
	gnss_fix_type = 0;
}

void GPSDisplay::gnssSolutionUpdated(const GnssSolutionSnapshot& solution)
{
	applyGnssSolution(solution, true);
	updateMapWidget();
}


void GPSDisplay::gnssStateChanged(const GnssState& state)
{
	Q_UNUSED(state);
	const bool had_live_position = has_valid_position;
	refreshFromGnssSession();
	if (had_live_position && !has_valid_position)
		emit positionUpdatesInterrupted();
	updateMapWidget();
}


void GPSDisplay::errorOccurred(QGeoPositionInfoSource::Error positioningError)
{
	Q_UNUSED(positioningError)
	if (source->error() != QGeoPositionInfoSource::NoError)
	{
		const bool had_live_position = has_valid_position;
		has_valid_position = false;
		if (has_display_position)
			tracking_lost = true;
		if (had_live_position)
		{
			emit positionUpdatesInterrupted();
			updateMapWidget();
		}
	}
}

void GPSDisplay::resetPositionState(bool clear_display_position)
{
	has_valid_position = false;
	tracking_lost = false;
	if (clear_display_position)
	{
		has_display_position = false;
		latest_gps_coord = MapCoordF();
	}
	latest_gps_coord_accuracy = -1.0f;
	gnss_fix_type = 0;
	gnss_h_accuracy_p95 = -1;
	gnss_ellipse_available = false;
	gnss_ellipse_semi_major = -1;
	gnss_ellipse_semi_minor = -1;
	gnss_ellipse_orientation = 0;
	gnss_latitude = 0;
	gnss_longitude = 0;
	gnss_altitude = -9999;
}

void GPSDisplay::updateMapWidget()
{
	// TODO: Limit update region to union of old and new bounding rect
	widget->update();
}


}  // namespace OpenOrienteering
