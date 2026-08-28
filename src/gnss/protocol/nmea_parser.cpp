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

// minmea_tocoord() returns float, which quantizes WGS84 positions by roughly
// half a metre at field latitudes. Preserve the sentence's actual precision.
double toCoordDouble(const struct minmea_float* value)
{
	if (value->scale == 0 || value->scale > (INT_LEAST32_MAX / 100)
	    || value->scale < (INT_LEAST32_MIN / 100))
	{
		return NAN;
	}
	const auto degrees = value->value / (value->scale * 100);
	const auto minutes = value->value % (value->scale * 100);
	return double(degrees) + double(minutes) / (60.0 * value->scale);
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
	m_gstValid = false;
	m_gstHours = m_gstMinutes = m_gstSeconds = -1;
	m_gstHAccuracy = m_gstVAccuracy = NAN;
	m_epeHAccuracy = m_epeVAccuracy = NAN;
	m_epeArrivalMs = 0;
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

	// Proprietary sentences ($P...) have no talker ID and are invisible to
	// minmea's sentence table, so they are dispatched by full identifier first.
	if (sentence.size() > 2 && sentence[1] == 'P')
	{
		if (handleProprietary(sentence))
			return;
	}

	// Determine sentence type (minmea uses the talker-agnostic ID)
	auto sentenceId = minmea_sentence_id(sentence.constData(), false);

	switch (sentenceId) {
	case MINMEA_SENTENCE_GGA: handleGGA(sentence.constData()); break;
	case MINMEA_SENTENCE_RMC: handleRMC(sentence.constData()); break;
	case MINMEA_SENTENCE_GSA: handleGSA(sentence.constData()); break;
	case MINMEA_SENTENCE_GSV: handleGSV(sentence.constData()); break;
	case MINMEA_SENTENCE_GST: handleGST(sentence.constData()); break;
	default: break;
	}
}


bool NmeaParser::handleProprietary(const QByteArray& sentence)
{
	auto fields = splitSentenceFields(sentence.constData());
	if (fields.isEmpty())
		return false;

	const auto identifier = fields.first();
	if (identifier == "PQTMDRPVA")
	{
		emit sentenceDecoded(QStringLiteral("PQTMDRPVA"));
		handleDrPva(fields);
		return true;
	}
	if (identifier == "PQTMDRCAL")
	{
		emit sentenceDecoded(QStringLiteral("PQTMDRCAL"));
		handleDrCal(fields);
		return true;
	}
	if (identifier == "PQTMEPE")
	{
		emit sentenceDecoded(QStringLiteral("PQTMEPE"));
		handleEpe(fields);
		return true;
	}
	if (identifier == "PQTMTXT")
	{
		emit sentenceDecoded(QStringLiteral("PQTMTXT"));
		// $PQTMTXT,<MsgVer>,<TotalSentences>,<Index>,<Severity>,<text>...
		if (fields.size() > 5)
		{
			QByteArrayList text = fields.mid(5);
			emit receiverStatusText(QString::fromLatin1(text.join(',')));
		}
		return true;
	}
	if (identifier == "PAIR001")
	{
		emit sentenceDecoded(QStringLiteral("PAIR001"));
		if (fields.size() >= 3)
		{
			bool id_ok = false;
			bool result_ok = false;
			const int command_id = fields.at(1).toInt(&id_ok);
			const int result = fields.at(2).toInt(&result_ok);
			if (id_ok && result_ok)
				emit commandAcknowledged(command_id, result);
		}
		return true;
	}
	if (identifier.startsWith("PQTM") || identifier.startsWith("PAIR"))
	{
		// Recognized family, no decoder yet: still count it so message-rate
		// statistics and the diagnostics panel show the real sentence mix.
		//
		// Only sentences that carried a checksum are counted. minmea_check()
		// accepts a checksum-less sentence, and a receiver really does emit
		// truncated ones: a GEO-PULSE restarting its GNSS engine cuts the
		// stream mid-sentence, leaving fragments such as "$PAIR0". Counting
		// those would fill the bounded statistics table with noise.
		if (!sentence.contains('*'))
			return true;
		emit sentenceDecoded(QString::fromLatin1(identifier));
		return true;
	}
	return false;
}


void NmeaParser::handleDrPva(const QByteArrayList& fields)
{
	// $PQTMDRPVA,<MsgVer>,<Timestamp>,<Time>,<SolType>,<Lat>,<Lon>,<Alt>,<Sep>,
	//            <VelN>,<VelE>,<VelD>,<Spd>,<Roll>,<Pitch>,<Heading>
	// Every value field is empty when invalid.
	constexpr int kFieldCount = 16;
	if (fields.size() < kFieldCount)
		return;

	auto number = [&fields](int index) -> float {
		bool ok = false;
		const auto value = fields.at(index).toFloat(&ok);
		return ok ? value : NAN;
	};

	// PQTMDRPVA's SolType and PQTMDRCAL's NavType disagree on the meaning of
	// 2 and 3: SolType 2 is GNSS+DR and 3 is DR-only, while NavType 2 is
	// DR-only and 3 is GNSS+DR. GnssDeadReckoningObservation carries the
	// NavType encoding, so SolType 2 and 3 are swapped on the way in.
	bool sol_ok = false;
	const int solution_type = fields.at(4).toInt(&sol_ok);
	int navigation_type = sol_ok ? solution_type : -1;
	if (navigation_type == 2)
		navigation_type = 3;
	else if (navigation_type == 3)
		navigation_type = 2;

	GnssDeadReckoningObservation observation;
	observation.meta.source = GnssObservationSource::QuectelDrPva;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.navigationType = navigation_type;
	observation.roll = number(13);
	observation.pitch = number(14);
	observation.heading = number(15);

	switch (observation.navigationType) {
	case 0:
		observation.meta.limitation = QStringLiteral("Receiver reports no dead-reckoning solution");
		break;
	case 2:
		observation.meta.limitation = QStringLiteral("Dead-reckoning-only solution; no GNSS fix");
		break;
	case 3:
		observation.meta.limitation = QStringLiteral("Solution fused from GNSS and dead reckoning");
		break;
	default:
		break;
	}

	emit deadReckoningObservation(observation);
}


void NmeaParser::handleEpe(const QByteArrayList& fields)
{
	// $PQTMEPE,<MsgVer>,<EPE_North>,<EPE_East>,<EPE_Down>,<EPE_2D>,<EPE_3D>
	// All values in meters. Verified live on the GEO-PULSE (LC29HBA).
	// fields[0] is the sentence identifier itself.
	if (fields.size() < 7)
		return;

	auto number = [&fields](int index) -> float {
		bool ok = false;
		const auto value = fields.at(index).toFloat(&ok);
		return ok ? value : NAN;
	};

	m_epeHAccuracy = number(5);  // 2D error
	m_epeVAccuracy = number(4);  // down-component error
	m_epeArrivalMs = QDateTime::currentMSecsSinceEpoch();
}


void NmeaParser::handleDrCal(const QByteArrayList& fields)
{
	// $PQTMDRCAL,<MsgVer>,<CalState>,<NavType>
	if (fields.size() < 4)
		return;

	bool cal_ok = false;
	bool nav_ok = false;
	const int calibration_state = fields.at(2).toInt(&cal_ok);
	const int navigation_type = fields.at(3).toInt(&nav_ok);

	GnssDeadReckoningObservation observation;
	observation.meta.source = GnssObservationSource::QuectelDrCal;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.calibrationState = cal_ok ? calibration_state : -1;
	observation.navigationType = nav_ok ? navigation_type : -1;

	emit deadReckoningObservation(observation);
}


void NmeaParser::handleGGA(const char* sentence)
{
	struct minmea_sentence_gga gga;
	if (!minmea_parse_gga(&gga, sentence))
		return;

	GnssPositionObservation observation;
	observation.meta.source = GnssObservationSource::NmeaGga;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.position.latitude = toCoordDouble(&gga.latitude);
	observation.position.longitude = toCoordDouble(&gga.longitude);

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

	if (gga.hdop.scale != 0)
		observation.position.hDOP = minmea_tofloat(&gga.hdop);

	// GGA has no error estimate. Prefer receiver GST statistics from the same
	// epoch; otherwise derive a conservative estimate appropriate to fix type.
	const auto gst_matches_epoch = m_gstValid
	    && m_gstHours == gga.time.hours
	    && m_gstMinutes == gga.time.minutes
	    && m_gstSeconds == gga.time.seconds;
	if (gst_matches_epoch)
	{
		observation.position.hAccuracy = m_gstHAccuracy;
		observation.position.vAccuracy = m_gstVAccuracy;
		observation.position.accuracyBasis = GnssAccuracyBasis::Sigma68;
		observation.position.computeP95();
		observation.meta.accuracyDerived = false;
		observation.meta.limitation = mergeLimitation(
		    observation.meta.limitation,
		    QStringLiteral("Horizontal accuracy from GST error statistics"));
	}
	else if (!std::isnan(m_epeHAccuracy)
	         && QDateTime::currentMSecsSinceEpoch() - m_epeArrivalMs <= kEpeFreshMs)
	{
		// Receiver-estimated position error. No timestamp on the sentence,
		// so freshness is by arrival; at worst the estimate is one epoch old.
		observation.position.hAccuracy = m_epeHAccuracy;
		observation.position.vAccuracy = m_epeVAccuracy;
		observation.position.accuracyBasis = GnssAccuracyBasis::Unknown;
		observation.position.computeP95();
		observation.meta.accuracyDerived = false;
		observation.meta.limitation = mergeLimitation(
		    observation.meta.limitation,
		    QStringLiteral("Accuracy from receiver-estimated position error (PQTMEPE)"));
	}
	else if (!std::isnan(observation.position.hDOP))
	{
		float uere = 2.0f;
		switch (observation.position.fixType)
		{
		case GnssFixType::RtkFixed: uere = 0.05f; break;
		case GnssFixType::RtkFloat: uere = 0.5f; break;
		case GnssFixType::DGPS: uere = 1.0f; break;
		default: break;
		}
		observation.position.hAccuracy = observation.position.hDOP * uere;
		observation.position.accuracyBasis = GnssAccuracyBasis::Sigma68;
		observation.position.computeP95();
		observation.meta.accuracyDerived = true;
		observation.meta.limitation = mergeLimitation(
		    observation.meta.limitation,
		    QStringLiteral("Horizontal accuracy derived from HDOP with fix-type UERE"));
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
	observation.position.latitude = toCoordDouble(&rmc.latitude);
	observation.position.longitude = toCoordDouble(&rmc.longitude);
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


void NmeaParser::handleGST(const char* sentence)
{
	struct minmea_sentence_gst gst;
	if (!minmea_parse_gst(&gst, sentence))
		return;

	m_gstValid = false;
	if (gst.latitude_error_deviation.scale == 0
	    || gst.longitude_error_deviation.scale == 0)
	{
		return;
	}

	const auto latitude_error = minmea_tofloat(&gst.latitude_error_deviation);
	const auto longitude_error = minmea_tofloat(&gst.longitude_error_deviation);
	if (!(latitude_error >= 0.0f) || !(longitude_error >= 0.0f))
		return;

	m_gstHAccuracy = std::hypot(latitude_error, longitude_error);
	m_gstVAccuracy = gst.altitude_error_deviation.scale != 0
	                     ? minmea_tofloat(&gst.altitude_error_deviation)
	                     : NAN;
	m_gstHours = gst.time.hours;
	m_gstMinutes = gst.time.minutes;
	m_gstSeconds = gst.time.seconds;
	m_gstValid = true;
}


}  // namespace OpenOrienteering
