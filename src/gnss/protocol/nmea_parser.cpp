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

#include "nmea_parser.h"

#include <cmath>
#include <cstring>

#include <QTimeZone>

#include "gnss/3rdparty/minmea/minmea.h"

namespace OpenOrienteering {


NmeaParser::NmeaParser(QObject* parent)
    : QObject(parent)
{
	m_lineBuffer.reserve(kMaxLineLength);
}

NmeaParser::~NmeaParser() = default;


void NmeaParser::addData(const QByteArray& data)
{
	m_stats.bytesProcessed += data.size();

	for (char c : data)
	{
		if (c == '\n')
		{
			if (!m_lineBuffer.isEmpty())
			{
				// Strip trailing CR if present
				if (m_lineBuffer.endsWith('\r'))
					m_lineBuffer.chop(1);
				processSentence(m_lineBuffer);
				m_lineBuffer.clear();
			}
		}
		else
		{
			m_lineBuffer.append(c);
			if (m_lineBuffer.size() > kMaxLineLength)
				m_lineBuffer.clear();  // Runaway line, discard
		}
	}
}


void NmeaParser::reset()
{
	m_lineBuffer.clear();
	m_pendingPosition = {};
	m_hasGGA = false;
	m_stats = {};
}


void NmeaParser::processSentence(const QByteArray& sentence)
{
	if (sentence.isEmpty() || sentence[0] != '$')
		return;

	// Validate checksum via minmea
	if (!minmea_check(sentence.constData(), false))
	{
		++m_stats.checksumErrors;
		return;
	}

	++m_stats.sentencesParsed;

	// Determine sentence type (minmea uses the talker-agnostic ID)
	auto sentenceId = minmea_sentence_id(sentence.constData(), false);

	switch (sentenceId) {
	case MINMEA_SENTENCE_GGA: handleGGA(sentence.constData()); break;
	case MINMEA_SENTENCE_RMC: handleRMC(sentence.constData()); break;
	case MINMEA_SENTENCE_GSA: handleGSA(sentence.constData()); break;
	case MINMEA_SENTENCE_GSV: handleGSV(sentence.constData()); break;
	default: break;
	}
}


void NmeaParser::handleGGA(const char* sentence)
{
	struct minmea_sentence_gga gga;
	if (!minmea_parse_gga(&gga, sentence))
		return;

	m_pendingPosition = {};
	m_pendingPosition.latitude  = minmea_tocoord(&gga.latitude);
	m_pendingPosition.longitude = minmea_tocoord(&gga.longitude);

	// Altitude above MSL
	if (gga.altitude.scale != 0)
		m_pendingPosition.altitudeMsl = minmea_tofloat(&gga.altitude);

	// Geoid separation
	if (gga.height.scale != 0)
	{
		m_pendingPosition.geoidSeparation = minmea_tofloat(&gga.height);
		if (!std::isnan(m_pendingPosition.altitudeMsl))
			m_pendingPosition.altitude = m_pendingPosition.altitudeMsl + m_pendingPosition.geoidSeparation;
	}

	// Fix quality → fix type
	switch (gga.fix_quality) {
	case 0:  m_pendingPosition.fixType = GnssFixType::NoFix;    break;
	case 1:  m_pendingPosition.fixType = GnssFixType::Fix3D;    break;
	case 2:  m_pendingPosition.fixType = GnssFixType::DGPS;     break;
	case 4:  m_pendingPosition.fixType = GnssFixType::RtkFixed; break;
	case 5:  m_pendingPosition.fixType = GnssFixType::RtkFloat; break;
	default: m_pendingPosition.fixType = GnssFixType::Fix3D;    break;
	}

	m_pendingPosition.valid = (gga.fix_quality > 0);
	m_pendingPosition.satellitesUsed = static_cast<std::uint8_t>(gga.satellites_tracked);

	// HDOP — we can derive a rough accuracy estimate: hAcc ~ HDOP * 2.5m (typical UERE)
	if (gga.hdop.scale != 0)
	{
		m_pendingPosition.hDOP = minmea_tofloat(&gga.hdop);
		// Use HDOP-derived accuracy only if we have no better source.
		// UERE ~2.5m for single-frequency, ~1.5m for dual-frequency.
		// We use 2.0m as a reasonable middle ground.
		m_pendingPosition.hAccuracy = m_pendingPosition.hDOP * 2.0f;
		m_pendingPosition.accuracyBasis = GnssAccuracyBasis::Sigma68;
		m_pendingPosition.computeP95();
	}

	// Correction age
	if (gga.dgps_age.scale != 0)
		m_pendingPosition.correctionAge = minmea_tofloat(&gga.dgps_age);

	// Build timestamp from GGA time (time only, no date — RMC has date)
	if (gga.time.hours >= 0)
	{
		QTime time(gga.time.hours, gga.time.minutes, gga.time.seconds,
		           gga.time.microseconds / 1000);
		if (time.isValid())
		{
			m_pendingPosition.timestamp = QDateTime(QDate::currentDate(), time, QTimeZone::UTC);
		}
	}

	m_hasGGA = true;
	emit positionUpdated(m_pendingPosition);
}


void NmeaParser::handleRMC(const char* sentence)
{
	struct minmea_sentence_rmc rmc;
	if (!minmea_parse_rmc(&rmc, sentence))
		return;

	// If we have a pending GGA position, enrich it with RMC date and speed.
	// Otherwise, build a position from RMC alone (less detail).
	GnssPosition& pos = m_hasGGA ? m_pendingPosition : (m_pendingPosition = {});

	if (!m_hasGGA)
	{
		pos.latitude  = minmea_tocoord(&rmc.latitude);
		pos.longitude = minmea_tocoord(&rmc.longitude);
		pos.valid = rmc.valid;
		pos.fixType = rmc.valid ? GnssFixType::Fix3D : GnssFixType::NoFix;
	}

	// Speed and course
	if (rmc.speed.scale != 0)
		pos.groundSpeed = minmea_tofloat(&rmc.speed) * 0.514444f;  // knots → m/s
	if (rmc.course.scale != 0)
		pos.headingMotion = minmea_tofloat(&rmc.course);

	// Date + time for a full timestamp
	if (rmc.date.year > 0 && rmc.time.hours >= 0)
	{
		QDate date(2000 + rmc.date.year, rmc.date.month, rmc.date.day);
		QTime time(rmc.time.hours, rmc.time.minutes, rmc.time.seconds,
		           rmc.time.microseconds / 1000);
		if (date.isValid() && time.isValid())
			pos.timestamp = QDateTime(date, time, QTimeZone::UTC);
	}

	if (!m_hasGGA && pos.valid)
		emit positionUpdated(pos);

	m_hasGGA = false;
}


void NmeaParser::handleGSA(const char* sentence)
{
	struct minmea_sentence_gsa gsa;
	if (!minmea_parse_gsa(&gsa, sentence))
		return;

	float pDOP = NAN, hDOP = NAN, vDOP = NAN;
	if (gsa.pdop.scale != 0) pDOP = minmea_tofloat(&gsa.pdop);
	if (gsa.hdop.scale != 0) hDOP = minmea_tofloat(&gsa.hdop);
	if (gsa.vdop.scale != 0) vDOP = minmea_tofloat(&gsa.vdop);

	emit dopUpdated(pDOP, hDOP, vDOP);
}


void NmeaParser::handleGSV(const char* sentence)
{
	struct minmea_sentence_gsv gsv;
	if (!minmea_parse_gsv(&gsv, sentence))
		return;

	emit satelliteInfoUpdated(gsv.total_sats);
}


}  // namespace OpenOrienteering
