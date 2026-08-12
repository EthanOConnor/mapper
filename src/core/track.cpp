/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2014-2018 Kai Pastor
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


#include "track.h"

#include <memory>

#include <Qt>
#include <QtGlobal>
#include <QtNumeric>
#include <QApplication>
#include <QFile>
#include <QFileInfo>  // IWYU pragma: keep
#include <QIODevice>
#include <QLatin1String>
#include <QPoint>
#include <QPointF>
#include <QSaveFile>
#include <QStringView>
#include <QXmlStreamAttributes>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
// IWYU pragma: no_include <qxmlstream.h>

#include "core/georeferencing.h"
#include "core/storage_location.h"  // IWYU pragma: keep


namespace OpenOrienteering {

namespace {

constexpr auto gpx_namespace = QLatin1String("http://www.topografix.com/GPX/1/1");
constexpr auto mapper_gnss_namespace = QLatin1String("http://openorienteering.org/xmlns/mapper-gnss/1");

/// The best approximation of the fix type in the standard GPX `fixType` vocabulary.
QString toGpxFixString(TrackFixType fix_type)
{
	switch (fix_type)
	{
	case TrackFixType::None:
		return QStringLiteral("none");
	case TrackFixType::Fix2D:
		return QStringLiteral("2d");
	case TrackFixType::Fix3D:
		return QStringLiteral("3d");
	case TrackFixType::DGPS:
	case TrackFixType::RtkFloat:
	case TrackFixType::RtkFixed:
		// RTK is differentially corrected; "dgps" is the best standard value.
		return QStringLiteral("dgps");
	case TrackFixType::Unknown:
		break;
	}
	return {};
}

/// The full fix type vocabulary of the mapper-gnss extension namespace.
QString toMapperFixString(TrackFixType fix_type)
{
	switch (fix_type)
	{
	case TrackFixType::None:
		return QStringLiteral("none");
	case TrackFixType::Fix2D:
		return QStringLiteral("2d");
	case TrackFixType::Fix3D:
		return QStringLiteral("3d");
	case TrackFixType::DGPS:
		return QStringLiteral("dgps");
	case TrackFixType::RtkFloat:
		return QStringLiteral("rtk-float");
	case TrackFixType::RtkFixed:
		return QStringLiteral("rtk-fixed");
	case TrackFixType::Unknown:
		break;
	}
	return {};
}

/// Parses both the standard GPX `fixType` vocabulary and the mapper-gnss one.
TrackFixType fixTypeFromString(QStringView string)
{
	if (string.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0)
		return TrackFixType::None;
	if (string.compare(QLatin1String("2d"), Qt::CaseInsensitive) == 0)
		return TrackFixType::Fix2D;
	if (string.compare(QLatin1String("3d"), Qt::CaseInsensitive) == 0
	    || string.compare(QLatin1String("pps"), Qt::CaseInsensitive) == 0)
		return TrackFixType::Fix3D;
	if (string.compare(QLatin1String("dgps"), Qt::CaseInsensitive) == 0)
		return TrackFixType::DGPS;
	if (string.compare(QLatin1String("rtk-float"), Qt::CaseInsensitive) == 0)
		return TrackFixType::RtkFloat;
	if (string.compare(QLatin1String("rtk-fixed"), Qt::CaseInsensitive) == 0)
		return TrackFixType::RtkFixed;
	return TrackFixType::Unknown;
}

}  // namespace


// ### TrackPoint ###

void TrackPoint::save(QXmlStreamWriter* stream, const QString* waypoint_name) const
{
	stream->writeAttribute(QStringLiteral("lat"), QString::number(latlon.latitude(), 'f', 12));
	stream->writeAttribute(QStringLiteral("lon"), QString::number(latlon.longitude(), 'f', 12));

	// Child elements in GPX 1.1 ptType schema order:
	// ele, time, ..., name, ..., fix, sat, hdop, ..., extensions.
	if (!qIsNaN(elevation))
		stream->writeTextElement(QStringLiteral("ele"), QString::number(static_cast<qreal>(elevation), 'f', 3));
	if (datetime.isValid())
		// Millisecond precision matters for multi-Hz GNSS logs (whole-second
		// stamps collide at 2.5Hz), but whole-second times keep the historic
		// format so existing consumers and fixtures see unchanged output.
		stream->writeTextElement(QStringLiteral("time"),
		                         datetime.time().msec() != 0
		                             ? datetime.toString(Qt::ISODateWithMs)
		                             : datetime.toString(Qt::ISODate));
	if (waypoint_name)
		stream->writeTextElement(QStringLiteral("name"), *waypoint_name);
	if (fixType != TrackFixType::Unknown)
		stream->writeTextElement(QStringLiteral("fix"), toGpxFixString(fixType));
	if (satellitesUsed >= 0)
		stream->writeTextElement(QStringLiteral("sat"), QString::number(satellitesUsed));
	// <hdop> carries true DOP only - never accuracy in meters.
	if (!qIsNaN(hDOP))
		stream->writeTextElement(QStringLiteral("hdop"), QString::number(static_cast<qreal>(hDOP), 'f', 3));

	const auto beyond_standard_fix = fixType == TrackFixType::RtkFloat
	                                 || fixType == TrackFixType::RtkFixed;
	if (!qIsNaN(hAccuracy) || !qIsNaN(vAccuracy) || !qIsNaN(correctionAge)
	    || !accuracyBasis.isEmpty() || beyond_standard_fix)
	{
		stream->writeStartElement(QStringLiteral("extensions"));
		stream->writeStartElement(mapper_gnss_namespace, QStringLiteral("gnss"));
		if (!qIsNaN(hAccuracy))
			stream->writeAttribute(QStringLiteral("hacc"), QString::number(static_cast<qreal>(hAccuracy), 'f', 3));
		if (!qIsNaN(vAccuracy))
			stream->writeAttribute(QStringLiteral("vacc"), QString::number(static_cast<qreal>(vAccuracy), 'f', 3));
		if (fixType != TrackFixType::Unknown)
			stream->writeAttribute(QStringLiteral("fix"), toMapperFixString(fixType));
		if (!accuracyBasis.isEmpty())
			stream->writeAttribute(QStringLiteral("basis"), accuracyBasis);
		if (!qIsNaN(correctionAge))
			stream->writeAttribute(QStringLiteral("corr_age"), QString::number(static_cast<qreal>(correctionAge), 'f', 3));
		stream->writeEndElement();  // mapper:gnss
		stream->writeEndElement();  // extensions
	}
}

bool operator==(const TrackPoint& lhs, const TrackPoint& rhs)
{
	auto fuzzyCompare =[](auto a, auto b) {
		return (qIsNaN(a) && qIsNaN(b))
		       || qFuzzyCompare(a, b);
	};
	return lhs.latlon == rhs.latlon
	       && lhs.map_coord == rhs.map_coord
	       && lhs.datetime == rhs.datetime
	       && fuzzyCompare(lhs.elevation, rhs.elevation)
	       && fuzzyCompare(lhs.hDOP, rhs.hDOP)
	       && fuzzyCompare(lhs.hAccuracy, rhs.hAccuracy)
	       && fuzzyCompare(lhs.vAccuracy, rhs.vAccuracy)
	       && fuzzyCompare(lhs.correctionAge, rhs.correctionAge)
	       && lhs.fixType == rhs.fixType
	       && lhs.satellitesUsed == rhs.satellitesUsed
	       && lhs.accuracyBasis == rhs.accuracyBasis;
}



// ### Track ###

Track::Track(const Georeferencing& map_georef)
: map_georef(map_georef)
{
	// nothing else
}

Track::Track(const Track& other)
{
	*this = other;
}

Track::~Track() = default;

Track& Track::operator=(const Track& rhs)
{
	if (this == &rhs)
		return *this;
	
	clear();
	
	waypoints = rhs.waypoints;
	waypoint_names = rhs.waypoint_names;
	
	segment_points = rhs.segment_points;
	segment_starts = rhs.segment_starts;
	
	current_segment_finished = rhs.current_segment_finished;
	
	map_georef = rhs.map_georef;
	
	return *this;
}

void Track::clear()
{
	waypoints.clear();
	waypoint_names.clear();
	segment_points.clear();
	segment_starts.clear();
	current_segment_finished = true;
}

bool Track::loadFrom(const QString& path, bool project_points)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return false;
	
	clear();
	if (path.endsWith(QLatin1String(".gpx"), Qt::CaseInsensitive))
	{
		if (!loadGpxFrom(file, project_points))
			return false;
	}
	else
		return false;

	file.close();
	return true;
}

bool Track::saveTo(const QString& path) const
{
	QSaveFile file(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Text)
	    && saveGpxTo(file)
	    && file.commit())
	{
		return true;  // NOLINT : redundant boolean literal in conditional return statement
	}
	
	return false;
}


bool Track::saveGpxTo(QIODevice& device) const
{
	static const auto newline = QString::fromLatin1("\n");
	
	QXmlStreamWriter stream(&device);
	stream.writeStartDocument();
	stream.writeCharacters(newline);
	stream.writeStartElement(QString::fromLatin1("gpx"));
	stream.writeAttribute(QString::fromLatin1("version"), QString::fromLatin1("1.1"));
	stream.writeAttribute(QString::fromLatin1("creator"), qApp->applicationDisplayName());
	stream.writeDefaultNamespace(gpx_namespace);
	stream.writeNamespace(mapper_gnss_namespace, QStringLiteral("mapper"));

	int size = getNumWaypoints();
	for (int i = 0; i < size; ++i)
	{
		stream.writeCharacters(newline);
		stream.writeStartElement(QStringLiteral("wpt"));
		const TrackPoint& point = getWaypoint(i);
		point.save(&stream, &getWaypointName(i));
		stream.writeEndElement();
	}
	
	stream.writeCharacters(newline);
	stream.writeStartElement(QStringLiteral("trk"));
	for (int i = 0; i < getNumSegments(); ++i)
	{
		stream.writeCharacters(newline);
		stream.writeStartElement(QStringLiteral("trkseg"));
		size = getSegmentPointCount(i);
		for (int k = 0; k < size; ++k)
		{
			stream.writeCharacters(newline);
			stream.writeStartElement(QStringLiteral("trkpt"));
			const TrackPoint& point = getSegmentPoint(i, k);
			point.save(&stream);
			stream.writeEndElement();
		}
		stream.writeCharacters(newline);
		stream.writeEndElement();
	}
	stream.writeCharacters(newline);
	stream.writeEndElement();
	
	stream.writeCharacters(newline);
	stream.writeEndElement();
	stream.writeEndDocument();
	return !stream.hasError();
}

void Track::appendTrackPoint(const TrackPoint& point)
{
	segment_points.push_back(point);
	segment_points.back().map_coord = map_georef.toMapCoordF(point.latlon, nullptr); // TODO: check for errors
	
	if (current_segment_finished)
	{
		segment_starts.push_back(segment_points.size() - 1);
		current_segment_finished = false;
	}
}
void Track::finishCurrentSegment()
{
	current_segment_finished = true;
}

void Track::appendWaypoint(const TrackPoint& point, const QString& name)
{
	waypoints.push_back(point);
	waypoints.back().map_coord = map_georef.toMapCoordF(point.latlon, nullptr); // TODO: check for errors
	waypoint_names.push_back(name);
}

void Track::changeMapGeoreferencing(const Georeferencing& new_map_georef)
{
	map_georef = new_map_georef;
	
	projectPoints();
}

int Track::getNumSegments() const
{
	return (int)segment_starts.size();
}

int Track::getSegmentPointCount(int segment_number) const
{
	Q_ASSERT(segment_number >= 0 && segment_number < (int)segment_starts.size());
	if (segment_number == (int)segment_starts.size() - 1)
		return segment_points.size() - segment_starts[segment_number];
	else
		return segment_starts[segment_number + 1] - segment_starts[segment_number];
}

const TrackPoint& Track::getSegmentPoint(int segment_number, int point_number) const
{
	Q_ASSERT(segment_number >= 0 && segment_number < (int)segment_starts.size());
	return segment_points[segment_starts[segment_number] + point_number];
}

int Track::getNumWaypoints() const
{
	return waypoints.size();
}

const TrackPoint& Track::getWaypoint(int number) const
{
	return waypoints[number];
}

const QString& Track::getWaypointName(int number) const
{
	return waypoint_names[number];
}

LatLon Track::calcAveragePosition() const
{
	double avg_latitude = 0;
	double avg_longitude = 0;
	int num_samples = 0;
	
	int size = getNumWaypoints();
	for (int i = 0; i < size; ++i)
	{
		const TrackPoint& point = getWaypoint(i);
		avg_latitude += point.latlon.latitude();
		avg_longitude += point.latlon.longitude();
		++num_samples;
	}
	for (int i = 0; i < getNumSegments(); ++i)
	{
		size = getSegmentPointCount(i);
		for (int k = 0; k < size; ++k)
		{
			const TrackPoint& point = getSegmentPoint(i, k);
			avg_latitude += point.latlon.latitude();
			avg_longitude += point.latlon.longitude();
			++num_samples;
		}
	}
	
	return LatLon((num_samples > 0) ? (avg_latitude / num_samples) : 0,
				  (num_samples > 0) ? (avg_longitude / num_samples) : 0);
}

bool Track::loadGpxFrom(QIODevice& device, bool project_points)
{
	TrackPoint point;
	QString point_name;

	QXmlStreamReader stream(&device);
	while (!stream.atEnd())
	{
		stream.readNext();
		if (stream.tokenType() == QXmlStreamReader::StartElement)
		{
			if (stream.name().compare(QLatin1String("wpt"), Qt::CaseInsensitive) == 0
			    || stream.name().compare(QLatin1String("trkpt"), Qt::CaseInsensitive) == 0
			    || stream.name().compare(QLatin1String("rtept"), Qt::CaseInsensitive) == 0)
			{
				point = TrackPoint{LatLon{stream.attributes().value(QLatin1String("lat")).toDouble(),
				                          stream.attributes().value(QLatin1String("lon")).toDouble()}};
				if (project_points)
					point.map_coord = map_georef.toMapCoordF(point.latlon); // TODO: check for errors
				point_name.clear();
			}
			else if (stream.name().compare(QLatin1String("trkseg"), Qt::CaseInsensitive) == 0
			         || stream.name().compare(QLatin1String("rte"), Qt::CaseInsensitive) == 0)
			{
				if (segment_starts.empty()
				    || segment_starts.back() < (int)segment_points.size())
				{
					segment_starts.push_back(segment_points.size());
				}
			}
			else if (stream.name().compare(QLatin1String("ele"), Qt::CaseInsensitive) == 0)
				point.elevation = stream.readElementText().toFloat();
			else if (stream.name().compare(QLatin1String("time"), Qt::CaseInsensitive) == 0)
				point.datetime = QDateTime::fromString(stream.readElementText(), Qt::ISODate);
			else if (stream.name().compare(QLatin1String("hdop"), Qt::CaseInsensitive) == 0)
				point.hDOP = stream.readElementText().toFloat();
			else if (stream.name().compare(QLatin1String("fix"), Qt::CaseInsensitive) == 0)
				point.fixType = fixTypeFromString(stream.readElementText());
			else if (stream.name().compare(QLatin1String("sat"), Qt::CaseInsensitive) == 0)
			{
				bool sat_ok = false;
				const auto sat = stream.readElementText().toInt(&sat_ok);
				point.satellitesUsed = (sat_ok && sat >= 0) ? sat : -1;
			}
			else if (stream.name().compare(QLatin1String("name"), Qt::CaseInsensitive) == 0)
				point_name = stream.readElementText();
			else if (stream.name().compare(QLatin1String("extensions"), Qt::CaseInsensitive) == 0)
			{
				// Handle known extension elements, and skip unknown extension
				// content as complete subtrees so that nested elements (such
				// as a vendor's own <ele> or <time>) cannot be misparsed as
				// GPX point data.
				while (stream.readNextStartElement())
				{
					if (stream.name().compare(QLatin1String("gnss"), Qt::CaseInsensitive) == 0
					    && (stream.namespaceUri() == mapper_gnss_namespace
					        || stream.namespaceUri().isEmpty()))
					{
						const auto attributes = stream.attributes();
						if (attributes.hasAttribute(QLatin1String("hacc")))
							point.hAccuracy = attributes.value(QLatin1String("hacc")).toFloat();
						if (attributes.hasAttribute(QLatin1String("vacc")))
							point.vAccuracy = attributes.value(QLatin1String("vacc")).toFloat();
						if (attributes.hasAttribute(QLatin1String("fix")))
						{
							// The extension value is more precise than the standard
							// <fix> element (e.g. rtk-fixed vs. dgps), but an
							// unrecognized value must not discard the standard one.
							const auto extension_fix = fixTypeFromString(attributes.value(QLatin1String("fix")));
							if (extension_fix != TrackFixType::Unknown)
								point.fixType = extension_fix;
						}
						if (attributes.hasAttribute(QLatin1String("basis")))
							point.accuracyBasis = attributes.value(QLatin1String("basis")).toString();
						if (attributes.hasAttribute(QLatin1String("corr_age")))
							point.correctionAge = attributes.value(QLatin1String("corr_age")).toFloat();
					}
					stream.skipCurrentElement();
				}
			}
		}
		else if (stream.tokenType() == QXmlStreamReader::EndElement)
		{
			if (stream.name().compare(QLatin1String("wpt"), Qt::CaseInsensitive) == 0)
			{
				waypoints.push_back(point);
				waypoint_names.push_back(point_name);
			}
			else if (stream.name().compare(QLatin1String("trkpt"), Qt::CaseInsensitive) == 0
			         || stream.name().compare(QLatin1String("rtept"), Qt::CaseInsensitive) == 0)
			{
				segment_points.push_back(point);
			}
		}
	}
	
	if (!segment_starts.empty()
	    && segment_starts.back() == (int)segment_points.size())
	{
		segment_starts.pop_back();
	}
	
	return !stream.hasError();
}

void Track::projectPoints()
{
	/// \todo Check for errors from Georeferencing::toMapCoordF()
	for (auto& waypoint : waypoints)
		waypoint.map_coord = map_georef.toMapCoordF(waypoint.latlon, nullptr); 
	for (auto& segment_point : segment_points)
		segment_point.map_coord = map_georef.toMapCoordF(segment_point.latlon, nullptr); 
}


bool operator==(const Track& lhs, const Track& rhs)
{
	return lhs.waypoints == rhs.waypoints
	       && lhs.waypoint_names == rhs.waypoint_names
	       && lhs.segment_points == rhs.segment_points
	       && lhs.segment_starts == rhs.segment_starts
	       && lhs.current_segment_finished == rhs.current_segment_finished;
}


}  // namespace OpenOrienteering
