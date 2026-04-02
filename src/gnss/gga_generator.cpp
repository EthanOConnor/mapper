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

#include "gga_generator.h"

#include <cmath>
#include <cstdio>

#include <QDateTime>
#include <QTimeZone>

#include "gnss_position.h"

namespace OpenOrienteering {
namespace GgaGenerator {


/// Compute the XOR checksum for an NMEA sentence body (between '$' and '*').
static char computeChecksum(const char* body, int length)
{
	char checksum = 0;
	for (int i = 0; i < length; ++i)
		checksum ^= body[i];
	return checksum;
}


/// Convert decimal degrees to NMEA DDDMM.MMMMM format.
static void degreesToDddmm(double decimal, int* degrees, double* minutes)
{
	double abs_val = std::fabs(decimal);
	*degrees = static_cast<int>(abs_val);
	*minutes = (abs_val - *degrees) * 60.0;
}


/// Map GnssFixType to NMEA GGA quality indicator.
static int fixTypeToGgaQuality(GnssFixType fixType)
{
	switch (fixType) {
	case GnssFixType::NoFix:    return 0;
	case GnssFixType::Fix2D:    return 1;
	case GnssFixType::Fix3D:    return 1;
	case GnssFixType::DGPS:     return 2;
	case GnssFixType::RtkFloat: return 5;
	case GnssFixType::RtkFixed: return 4;
	}
	return 0;
}


QByteArray fromPosition(const GnssPosition& pos)
{
	if (!pos.valid)
		return fromCoordinates(0, 0, 0, 0, 0, 99.9f, 0);

	int quality = fixTypeToGgaQuality(pos.fixType);
	double altMsl = std::isnan(pos.altitudeMsl) ? 0.0 : pos.altitudeMsl;
	float hdop = std::isnan(pos.hDOP) ? 99.9f : pos.hDOP;
	float geoidSep = std::isnan(pos.geoidSeparation) ? 0.0f : static_cast<float>(pos.geoidSeparation);
	int numSats = pos.satellitesUsed;

	return fromCoordinates(pos.latitude, pos.longitude,
	                       altMsl, quality, numSats, hdop, geoidSep);
}


QByteArray fromCoordinates(double latitude, double longitude,
                           double altitudeMsl, int quality,
                           int numSatellites, float hdop, float geoidSep)
{
	// NMEA GGA format:
	// $GPGGA,hhmmss.ss,ddmm.mmmmm,N,dddmm.mmmmm,E,q,nn,h.h,a.a,M,g.g,M,,*cc\r\n

	auto utc = QDateTime::currentDateTimeUtc();
	int hour = utc.time().hour();
	int min = utc.time().minute();
	int sec = utc.time().second();
	int msec = utc.time().msec();

	// Latitude → DDMM.MMMMM
	int latDeg;
	double latMin;
	degreesToDddmm(latitude, &latDeg, &latMin);
	char latHemi = (latitude >= 0) ? 'N' : 'S';

	// Longitude → DDDMM.MMMMM
	int lonDeg;
	double lonMin;
	degreesToDddmm(longitude, &lonDeg, &lonMin);
	char lonHemi = (longitude >= 0) ? 'E' : 'W';

	// Build sentence body (everything between $ and *)
	char body[256];
	int bodyLen = std::snprintf(body, sizeof(body),
		"GPGGA,%02d%02d%02d.%02d,%02d%010.7f,%c,%03d%010.7f,%c,%d,%02d,%.1f,%.1f,M,%.1f,M,,",
		hour, min, sec, msec / 10,
		latDeg, latMin, latHemi,
		lonDeg, lonMin, lonHemi,
		quality, numSatellites, static_cast<double>(hdop),
		altitudeMsl, static_cast<double>(geoidSep));

	if (bodyLen <= 0 || bodyLen >= static_cast<int>(sizeof(body)))
		return {};

	auto checksum = computeChecksum(body, bodyLen);

	// Assemble: $<body>*<checksum>\r\n
	char sentence[280];
	int sentenceLen = std::snprintf(sentence, sizeof(sentence),
		"$%s*%02X\r\n", body, static_cast<unsigned char>(checksum));

	return QByteArray(sentence, sentenceLen);
}


}  // namespace GgaGenerator
}  // namespace OpenOrienteering
