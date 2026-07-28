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

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QByteArrayList>
#include <QTimeZone>

#include "gnss/3rdparty/minmea/minmea.h"

namespace OpenOrienteering {

namespace {

QString mergeLimitation(const QString& left, const QString& right)
{
	if (left.isEmpty())
		return right;
	if (right.isEmpty() || left == right)
		return left;
	return left + QStringLiteral(" | ") + right;
}

QByteArrayList splitSentenceFields(const char* sentence)
{
	QByteArray raw(sentence);
	auto checksumPos = raw.indexOf('*');
	if (checksumPos >= 0)
		raw.truncate(checksumPos);
	if (!raw.isEmpty() && raw[0] == '$')
		raw.remove(0, 1);
	return raw.split(',');
}

float horizontalResolutionFromNmeaFields(const QByteArray& latField, const QByteArray& lonField, double latitude)
{
	constexpr double kPi = 3.14159265358979323846;

	auto fieldResolutionMinutes = [](const QByteArray& field) -> float {
		auto dot = field.indexOf('.');
		if (dot < 0)
			return field.isEmpty() ? NAN : 1.0f;
		auto decimals = field.size() - dot - 1;
		if (decimals < 0)
			return NAN;
		return std::pow(10.0f, static_cast<float>(-decimals));
	};

	float latResolutionMinutes = fieldResolutionMinutes(latField);
	float lonResolutionMinutes = fieldResolutionMinutes(lonField);
	if (std::isnan(latResolutionMinutes) && std::isnan(lonResolutionMinutes))
		return NAN;

	float latResolutionM = std::isnan(latResolutionMinutes) ? 0.0f : latResolutionMinutes * 1852.0f;
	float lonScale = std::cos(std::abs(latitude) * kPi / 180.0);
	float lonResolutionM = std::isnan(lonResolutionMinutes) ? 0.0f : lonResolutionMinutes * 1852.0f * lonScale;
	return std::max(latResolutionM, lonResolutionM);
}

}  // namespace


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

	GnssPositionObservation observation;
	observation.meta.source = GnssObservationSource::NmeaGga;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.position.latitude = minmea_tocoord(&gga.latitude);
	observation.position.longitude = minmea_tocoord(&gga.longitude);

	// Altitude above MSL
	if (gga.altitude.scale != 0)
		observation.position.altitudeMsl = minmea_tofloat(&gga.altitude);

	// Geoid separation
	if (gga.height.scale != 0)
	{
		observation.position.geoidSeparation = minmea_tofloat(&gga.height);
		if (!std::isnan(observation.position.altitudeMsl))
			observation.position.altitude = observation.position.altitudeMsl + observation.position.geoidSeparation;
	}

	// Fix quality → fix type
	switch (gga.fix_quality) {
	case 0:  observation.position.fixType = GnssFixType::NoFix;    break;
	case 1:  observation.position.fixType = GnssFixType::Fix3D;    break;
	case 2:  observation.position.fixType = GnssFixType::DGPS;     break;
	case 4:  observation.position.fixType = GnssFixType::RtkFixed; break;
	case 5:  observation.position.fixType = GnssFixType::RtkFloat; break;
	default: observation.position.fixType = GnssFixType::Fix3D;    break;
	}

	observation.position.valid = (gga.fix_quality > 0);
	observation.position.satellitesUsed = static_cast<std::uint8_t>(gga.satellites_tracked);

	// HDOP — we can derive a rough accuracy estimate: hAcc ~ HDOP * 2.5m (typical UERE)
	if (gga.hdop.scale != 0)
	{
		observation.position.hDOP = minmea_tofloat(&gga.hdop);
		// Use HDOP-derived accuracy only if we have no better source.
		// UERE ~2.5m for single-frequency, ~1.5m for dual-frequency.
		// We use 2.0m as a reasonable middle ground.
		observation.position.hAccuracy = observation.position.hDOP * 2.0f;
		observation.position.accuracyBasis = GnssAccuracyBasis::Sigma68;
		observation.position.computeP95();
		observation.meta.accuracyDerived = true;
		observation.meta.limitation = mergeLimitation(
		    observation.meta.limitation,
		    QStringLiteral("Horizontal accuracy derived from HDOP with assumed 2.0m UERE"));
	}

	// Correction age
	if (gga.dgps_age.scale != 0)
		observation.position.correctionAge = minmea_tofloat(&gga.dgps_age);

	// Build timestamp from GGA time (time only, no date — RMC has date)
	if (gga.time.hours >= 0)
	{
		QTime time(gga.time.hours, gga.time.minutes, gga.time.seconds,
		           gga.time.microseconds / 1000);
		if (time.isValid())
		{
			observation.position.timestamp = QDateTime(QDate::currentDate(), time, QTimeZone::UTC);
			observation.meta.timestampHasTime = true;
		}
	}
	observation.meta.timestampHasDate = false;
	observation.meta.limitation = mergeLimitation(
	    observation.meta.limitation,
	    QStringLiteral("NMEA GGA provides time-of-day only; date remains inferred until paired with RMC"));

	auto fields = splitSentenceFields(sentence);
	if (fields.size() > 5)
	{
		observation.meta.horizontalResolutionM = horizontalResolutionFromNmeaFields(
		    fields[2], fields[4], observation.position.latitude);
		if (!std::isnan(observation.meta.horizontalResolutionM))
		{
			observation.meta.limitation = mergeLimitation(
			    observation.meta.limitation,
			    QStringLiteral("Position granularity limited by NMEA field precision"));
		}
	}

	emit positionObservation(observation);
}


void NmeaParser::handleRMC(const char* sentence)
{
	struct minmea_sentence_rmc rmc;
	if (!minmea_parse_rmc(&rmc, sentence))
		return;

	GnssPositionObservation observation;
	observation.meta.source = GnssObservationSource::NmeaRmc;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.position.latitude = minmea_tocoord(&rmc.latitude);
	observation.position.longitude = minmea_tocoord(&rmc.longitude);
	observation.position.valid = rmc.valid;
	observation.position.fixType = rmc.valid ? GnssFixType::Fix3D : GnssFixType::NoFix;

	// Speed and course
	if (rmc.speed.scale != 0)
		observation.position.groundSpeed = minmea_tofloat(&rmc.speed) * 0.514444f;  // knots → m/s
	if (rmc.course.scale != 0)
		observation.position.headingMotion = minmea_tofloat(&rmc.course);

	// Date + time for a full timestamp
	if (rmc.date.year > 0 && rmc.time.hours >= 0)
	{
		QDate date(2000 + rmc.date.year, rmc.date.month, rmc.date.day);
		QTime time(rmc.time.hours, rmc.time.minutes, rmc.time.seconds,
		           rmc.time.microseconds / 1000);
		if (date.isValid() && time.isValid())
		{
			observation.position.timestamp = QDateTime(date, time, QTimeZone::UTC);
			observation.meta.timestampHasDate = true;
			observation.meta.timestampHasTime = true;
		}
	}

	auto fields = splitSentenceFields(sentence);
	if (fields.size() > 6)
	{
		observation.meta.horizontalResolutionM = horizontalResolutionFromNmeaFields(
		    fields[3], fields[5], observation.position.latitude);
	}

	observation.meta.limitation = QStringLiteral("NMEA RMC carries no altitude or direct accuracy estimate");
	if (!std::isnan(observation.meta.horizontalResolutionM))
	{
		observation.meta.limitation = mergeLimitation(
		    observation.meta.limitation,
		    QStringLiteral("Position granularity limited by NMEA field precision"));
	}

	emit positionObservation(observation);
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

	GnssDopObservation observation;
	observation.meta.source = GnssObservationSource::NmeaGsa;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.meta.limitation = QStringLiteral("NMEA GSA reports DOP only; no covariance or confidence basis");
	observation.pDOP = pDOP;
	observation.hDOP = hDOP;
	observation.vDOP = vDOP;
	emit dopObservation(observation);
}


void NmeaParser::handleGSV(const char* sentence)
{
	struct minmea_sentence_gsv gsv;
	if (!minmea_parse_gsv(&gsv, sentence))
		return;

	GnssSatelliteObservation observation;
	observation.meta.source = GnssObservationSource::NmeaGsv;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.meta.limitation = QStringLiteral("NMEA GSV reports visible satellites only");
	observation.satellitesVisible = gsv.total_sats;
	emit satelliteObservation(observation);
}


}  // namespace OpenOrienteering
