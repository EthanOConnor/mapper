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

#include "ubx_parser.h"

#if defined(MAPPER_GNSS_USE_GLEAN)
extern "C" {
#include <glean/ubx.h>
}
#endif

#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include <QTimeZone>
#include <QtGlobal>

#include "ubx_messages.h"

namespace OpenOrienteering {

#if defined(MAPPER_GNSS_USE_GLEAN)
struct UbxParser::GleanState
{
	GleanState()
	    : framer(glean_ubx_framer_new())
	{}

	~GleanState()
	{
		glean_ubx_framer_free(framer);
	}

	glean_ubx_framer_t* framer = nullptr;
	glean_position_t pendingPosition = {};
	glean_ubx_nav_hpposllh_t pendingHp = {};
	std::uint32_t pendingPositionItow = 0;
	GnssFixType pendingFixType = GnssFixType::NoFix;
	bool hasPendingPosition = false;
	bool hasPendingHp = false;
	bool previousByteWasSync1 = false;
	bool hasSkippedBytesBeforeNextFrame = false;
};
#endif


UbxParser::UbxParser(QObject* parent)
    : QObject(parent)
{
	m_buffer.reserve(4096);
#if defined(MAPPER_GNSS_USE_GLEAN)
	m_glean = new GleanState();
#endif
}

UbxParser::~UbxParser()
{
#if defined(MAPPER_GNSS_USE_GLEAN)
	delete m_glean;
#endif
}


void UbxParser::addData(const QByteArray& data)
{
	if (data.isEmpty())
		return;

#if defined(MAPPER_GNSS_USE_GLEAN)
	if (m_glean && m_glean->framer)
	{
		addDataWithGlean(data);
		return;
	}
#endif

	m_buffer.append(data);
	m_stats.bytesProcessed += data.size();

	// Prevent unbounded buffer growth when fed non-UBX data
	if (m_buffer.size() > kMaxBufferSize)
	{
		// Keep only the tail — valid frames might start near the end
		m_buffer = m_buffer.right(kMaxBufferSize / 2);
		++m_stats.syncResyncCount;
	}

	processBuffer();
}


void UbxParser::reset()
{
	m_buffer.clear();
	m_stats = {};
#if defined(MAPPER_GNSS_USE_GLEAN)
	if (m_glean)
	{
		m_glean->hasPendingPosition = false;
		m_glean->hasPendingHp = false;
		m_glean->pendingPositionItow = 0;
		m_glean->pendingFixType = GnssFixType::NoFix;
		m_glean->previousByteWasSync1 = false;
		m_glean->hasSkippedBytesBeforeNextFrame = false;
		if (m_glean->framer)
			glean_ubx_framer_reset(m_glean->framer);
	}
#endif
}


#if defined(MAPPER_GNSS_USE_GLEAN)

void UbxParser::addDataWithGlean(const QByteArray& data)
{
	m_stats.bytesProcessed += data.size();

	const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.constData());
	int syncOffset = -1;
	if (m_glean->previousByteWasSync1
	    && !data.isEmpty()
	    && bytes[0] == static_cast<std::uint8_t>(Ubx::kSyncChar2))
	{
		syncOffset = 0;
	}
	else
	{
		for (int i = 0; i < data.size() - 1; ++i)
		{
			if (bytes[i] == static_cast<std::uint8_t>(Ubx::kSyncChar1)
			    && bytes[i + 1] == static_cast<std::uint8_t>(Ubx::kSyncChar2))
			{
				syncOffset = i;
				break;
			}
		}
	}
	if (syncOffset >= 0)
	{
		if (syncOffset > 0 || m_glean->hasSkippedBytesBeforeNextFrame)
			++m_stats.syncResyncCount;
		m_glean->hasSkippedBytesBeforeNextFrame = false;
	}
	else if (data.size() > 1
	         || bytes[0] != static_cast<std::uint8_t>(Ubx::kSyncChar1))
	{
		m_glean->hasSkippedBytesBeforeNextFrame = true;
	}
	m_glean->previousByteWasSync1 =
	    !data.isEmpty() && bytes[data.size() - 1] == static_cast<std::uint8_t>(Ubx::kSyncChar1);

	auto err = glean_ubx_framer_feed(
	    m_glean->framer,
	    bytes,
	    static_cast<std::size_t>(data.size()));
	if (err != GLEAN_OK)
	{
		++m_stats.syncResyncCount;
		if (err == GLEAN_ERR_OUT_OF_MEMORY)
			glean_ubx_framer_reset(m_glean->framer);
		return;
	}

	processGleanFrames();
	m_stats.framesDecoded = glean_ubx_framer_good_frames(m_glean->framer);
	m_stats.checksumErrors = glean_ubx_framer_bad_frames(m_glean->framer);
}


void UbxParser::processGleanFrames()
{
	for (;;)
	{
		std::uint8_t msgClass = 0;
		std::uint8_t msgId = 0;
		const std::uint8_t* payload = nullptr;
		std::size_t payloadLen = 0;
		auto next = glean_ubx_framer_next(
		    m_glean->framer,
		    &msgClass,
		    &msgId,
		    &payload,
		    &payloadLen);
		if (next == 0)
			break;
		if (next < 0)
		{
			++m_stats.syncResyncCount;
			break;
		}

		emit rawFrame(
		    msgClass,
		    msgId,
		    QByteArray(reinterpret_cast<const char*>(payload), static_cast<int>(payloadLen)));
		dispatchGleanMessage(msgClass, msgId, payload, payloadLen);
	}
}


void UbxParser::dispatchGleanMessage(std::uint8_t msgClass, std::uint8_t msgId,
                                     const std::uint8_t* payload, std::size_t length)
{
	if (msgClass == GLEAN_UBX_CLASS_NAV)
	{
		switch (msgId) {
		case GLEAN_UBX_ID_NAV_PVT:
		{
			glean_ubx_nav_pvt_t pvt;
			if (glean_ubx_parse_nav_pvt(payload, length, &pvt) != GLEAN_OK)
				return;

			glean_position_t position;
			if (glean_ubx_position_from_pvt(&pvt, &position) != GLEAN_OK)
				return;

			if (m_glean->hasPendingHp && m_glean->pendingHp.itow_ms == pvt.itow_ms)
			{
				if (glean_ubx_position_merge_hpposllh(&position, &m_glean->pendingHp) == GLEAN_OK)
					m_glean->hasPendingHp = false;
			}

			m_glean->pendingPosition = position;
			m_glean->pendingPositionItow = pvt.itow_ms;
			m_glean->pendingFixType = classifyFix(pvt.fix_type, pvt.fix_flags);
			m_glean->hasPendingPosition = true;

			emitGleanPosition(pvt.itow_ms, m_glean->pendingFixType);
			return;
		}
		case GLEAN_UBX_ID_NAV_HPPOSLLH:
		{
			glean_ubx_nav_hpposllh_t hp;
			if (glean_ubx_parse_nav_hpposllh(payload, length, &hp) != GLEAN_OK)
				return;
			if ((hp.flags & GLEAN_UBX_HPPOSLLH_INVALID_LLH) != 0)
				return;

			if (m_glean->hasPendingPosition && m_glean->pendingPositionItow == hp.itow_ms)
			{
				if (glean_ubx_position_merge_hpposllh(&m_glean->pendingPosition, &hp) == GLEAN_OK)
					emitGleanPosition(hp.itow_ms, m_glean->pendingFixType);
			}
			else
			{
				m_glean->pendingHp = hp;
				m_glean->hasPendingHp = true;
			}
			return;
		}
		case GLEAN_UBX_ID_NAV_DOP:
		{
			glean_ubx_nav_dop_t dop;
			if (glean_ubx_parse_nav_dop(payload, length, &dop) != GLEAN_OK)
				return;

			GnssDopObservation observation;
			observation.meta.source = GnssObservationSource::UbxNavDop;
			observation.meta.observedAt = QDateTime::currentDateTimeUtc();
			observation.gDOP = dop.g_dop_1e2 * 0.01f;
			observation.pDOP = dop.p_dop_1e2 * 0.01f;
			observation.tDOP = dop.t_dop_1e2 * 0.01f;
			observation.vDOP = dop.v_dop_1e2 * 0.01f;
			observation.hDOP = dop.h_dop_1e2 * 0.01f;
			observation.nDOP = dop.n_dop_1e2 * 0.01f;
			observation.eDOP = dop.e_dop_1e2 * 0.01f;
			emit dopObservation(observation);
			return;
		}
		case GLEAN_UBX_ID_NAV_SAT:
		{
			std::vector<glean_ubx_nav_sat_entry_t> sats(64);
			glean_ubx_nav_sat_t navSat = {};
			navSat.sats = sats.data();
			navSat.sats_capacity = sats.size();
			auto err = glean_ubx_parse_nav_sat(payload, length, &navSat);
			if (err == GLEAN_ERR_BUFFER_TOO_SMALL && navSat.num_svs > sats.size())
			{
				sats.resize(navSat.num_svs);
				navSat.sats = sats.data();
				navSat.sats_capacity = sats.size();
				err = glean_ubx_parse_nav_sat(payload, length, &navSat);
			}
			if (err != GLEAN_OK)
				return;

			int totalUsed = 0;
			for (int i = 0; i < navSat.num_svs; ++i)
			{
				if ((sats[static_cast<std::size_t>(i)].flags & GLEAN_UBX_SAT_SV_USED) != 0)
					++totalUsed;
			}

			GnssSatelliteObservation observation;
			observation.meta.source = GnssObservationSource::UbxNavSat;
			observation.meta.observedAt = QDateTime::currentDateTimeUtc();
			observation.satellitesUsed = totalUsed;
			observation.satellitesVisible = navSat.num_svs;
			emit satelliteObservation(observation);
			return;
		}
		case GLEAN_UBX_ID_NAV_COV:
		{
			glean_ubx_nav_cov_t cov;
			if (glean_ubx_parse_nav_cov(payload, length, &cov) != GLEAN_OK)
				return;
			if (!cov.pos_cov_valid)
				return;

			GnssCovarianceObservation observation;
			observation.meta.source = GnssObservationSource::UbxNavCov;
			observation.meta.observedAt = QDateTime::currentDateTimeUtc();
			observation.covNN = cov.pos_cov_nn;
			observation.covNE = cov.pos_cov_ne;
			observation.covEE = cov.pos_cov_ee;
			emit covarianceObservation(observation);
			return;
		}
		case GLEAN_UBX_ID_NAV_STATUS:
		{
			glean_ubx_nav_status_t status;
			if (glean_ubx_parse_nav_status(payload, length, &status) != GLEAN_OK)
				return;

			GnssStatusObservation observation;
			observation.meta.source = GnssObservationSource::UbxNavStatus;
			observation.meta.observedAt = QDateTime::currentDateTimeUtc();
			observation.fixOK = (status.flags & GLEAN_UBX_STATUS_GPS_FIX_OK) != 0;
			observation.diffSoln = (status.flags & GLEAN_UBX_STATUS_DIFF_SOLN) != 0;
			observation.carrSoln = (status.flags2 >> 6) & 0x03;
			observation.spoofDet = (status.flags2 >> 3) & 0x03;
			emit statusObservation(observation);
			return;
		}
		default:
			break;
		}
	}
	else if (msgClass == GLEAN_UBX_CLASS_MON)
	{
		switch (msgId) {
		case GLEAN_UBX_ID_MON_VER:
		{
			glean_ubx_mon_ver_t ver;
			if (glean_ubx_parse_mon_ver(payload, length, &ver) != GLEAN_OK)
				return;

			GnssVersionObservation observation;
			observation.meta.source = GnssObservationSource::UbxMonVer;
			observation.meta.observedAt = QDateTime::currentDateTimeUtc();
			observation.swVersion = QString::fromLatin1(ver.sw_version);
			observation.hwVersion = QString::fromLatin1(ver.hw_version);
			for (std::uint8_t i = 0; i < ver.num_extensions; ++i)
			{
				auto ext = QString::fromLatin1(ver.extensions[i]);
				if (!ext.isEmpty())
					observation.extensions.append(ext);
			}
			emit versionObservation(observation);
			return;
		}
		default:
			break;
		}
	}

	++m_stats.unknownMessages;
}


void UbxParser::emitGleanPosition(std::uint32_t itowMs, GnssFixType fixType)
{
	Q_UNUSED(itowMs)
	const auto& source = m_glean->pendingPosition;
	if ((source.valid_mask & GLEAN_POS_HAS_GEODETIC) == 0)
		return;

	const bool hasHp = (source.valid_mask & GLEAN_POS_HAS_HP_EXTENSION) != 0;

	GnssPositionObservation observation;
	observation.meta.source = GnssObservationSource::UbxNavPvt;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.meta.timestampHasDate = (source.valid_mask & GLEAN_POS_HAS_UTC_TIME) != 0;
	observation.meta.timestampHasTime = observation.meta.timestampHasDate;
	observation.meta.horizontalResolutionM = hasHp ? 0.0002f : 0.012f;
	observation.meta.verticalResolutionM = hasHp ? 0.0002f : 0.001f;
	observation.meta.limitation = hasHp
	    ? QStringLiteral("u-blox NAV-PVT merged with NAV-HPPOSLLH by glean; accuracy values are receiver-estimated and treated as ~1-sigma")
	    : QStringLiteral("u-blox NAV-PVT decoded by glean; accuracy values are receiver-estimated and treated as ~1-sigma");

	auto& pos = observation.position;
	pos.fixType = fixType;
	pos.valid = (fixType != GnssFixType::NoFix);
	pos.latitude = source.lat_deg + (hasHp ? source.lat_hp_deg_extra : 0.0);
	pos.longitude = source.lon_deg + (hasHp ? source.lon_hp_deg_extra : 0.0);
	pos.altitude = source.alt_ellipsoid_m + (hasHp ? source.alt_hp_m_extra : 0.0);
	pos.altitudeMsl = source.alt_msl_m + (hasHp ? source.alt_hp_m_extra : 0.0);
	if ((source.valid_mask & GLEAN_POS_HAS_GEOID_SEPARATION) != 0)
		pos.geoidSeparation = source.geoid_sep_m;

	if ((source.valid_mask & GLEAN_POS_HAS_HORIZ_ACCURACY) != 0)
		pos.hAccuracy = source.horiz_acc_m;
	if ((source.valid_mask & GLEAN_POS_HAS_VERT_ACCURACY) != 0)
		pos.vAccuracy = source.vert_acc_m;
	pos.accuracyBasis = GnssAccuracyBasis::Sigma68;
	pos.computeP95();

	if ((source.valid_mask & GLEAN_POS_HAS_PDOP) != 0)
		pos.pDOP = source.pdop;
	if ((source.valid_mask & GLEAN_POS_HAS_HDOP) != 0)
		pos.hDOP = source.hdop;
	if ((source.valid_mask & GLEAN_POS_HAS_VDOP) != 0)
		pos.vDOP = source.vdop;
	if ((source.valid_mask & GLEAN_POS_HAS_SATS_USED) != 0)
		pos.satellitesUsed = static_cast<std::uint8_t>(qBound(0, source.sats_used, 255));

	if ((source.valid_mask & GLEAN_POS_HAS_GROUND_VELOCITY) != 0)
	{
		pos.groundSpeed = static_cast<float>(source.ground_speed_m_s);
		pos.headingMotion = static_cast<float>(source.course_over_ground_deg);
	}
	if ((source.valid_mask & GLEAN_POS_HAS_VELOCITY_ACCURACY) != 0)
		pos.speedAccuracy = source.speed_acc_m_s;

	if ((source.valid_mask & GLEAN_POS_HAS_UTC_TIME) != 0)
	{
		QDate date(source.utc_year, source.utc_month, source.utc_day);
		auto sec = static_cast<int>(std::floor(source.utc_sec));
		auto frac = source.utc_sec - sec;
		auto msec = static_cast<int>(std::lround(frac * 1000.0));
		if (msec >= 1000)
		{
			sec += 1;
			msec -= 1000;
		}
		auto time = QTime(source.utc_hour, source.utc_min, qBound(0, sec, 59), qBound(0, msec, 999));
		if (date.isValid() && time.isValid())
			pos.timestamp = QDateTime(date, time, QTimeZone::UTC);
	}

	emit positionObservation(observation);
}

#endif


void UbxParser::processBuffer()
{
	const char* buf = m_buffer.constData();
	int size = static_cast<int>(m_buffer.size());
	int pos = 0;

	while (pos < size)
	{
		// Scan for sync bytes
		int syncPos = -1;
		for (int i = pos; i < size - 1; ++i)
		{
			if (static_cast<std::uint8_t>(buf[i]) == Ubx::kSyncChar1
			    && static_cast<std::uint8_t>(buf[i + 1]) == Ubx::kSyncChar2)
			{
				syncPos = i;
				break;
			}
		}

		if (syncPos < 0)
		{
			// No sync found; discard everything except the last byte
			// (which could be 0xB5 waiting for 0x62)
			pos = size - 1;
			break;
		}

		if (syncPos > pos)
		{
			// Skipped bytes before sync — track resyncs
			++m_stats.syncResyncCount;
		}

		pos = syncPos;

		// Need at least the header: sync(2) + class(1) + id(1) + length(2) = 6 bytes
		if (pos + 6 > size)
			break;

		auto msgClass = static_cast<std::uint8_t>(buf[pos + 2]);
		auto msgId    = static_cast<std::uint8_t>(buf[pos + 3]);
		auto payloadLen = static_cast<std::uint16_t>(
		    static_cast<std::uint8_t>(buf[pos + 4])
		    | (static_cast<std::uint8_t>(buf[pos + 5]) << 8));

		int frameLen = Ubx::kFrameOverhead + payloadLen;

		// Sanity check: UBX payloads should not exceed ~8KB in practice.
		// A huge length likely means we're misaligned.
		if (payloadLen > 8192)
		{
			++m_stats.syncResyncCount;
			pos += 2;  // Skip past this false sync
			continue;
		}

		// Wait for the full frame
		if (pos + frameLen > size)
			break;

		// Verify Fletcher-8 checksum (computed over class, id, length, payload)
		auto [ckA, ckB] = fletcher8(buf + pos + 2, 4 + payloadLen);
		auto expectedCkA = static_cast<std::uint8_t>(buf[pos + 6 + payloadLen]);
		auto expectedCkB = static_cast<std::uint8_t>(buf[pos + 7 + payloadLen]);

		if (ckA != expectedCkA || ckB != expectedCkB)
		{
			++m_stats.checksumErrors;
			pos += 2;  // Skip past false sync, try again
			continue;
		}

		// Valid frame — dispatch it
		const char* payload = buf + pos + 6;
		++m_stats.framesDecoded;

		emit rawFrame(msgClass, msgId, QByteArray(payload, payloadLen));
		dispatchMessage(msgClass, msgId, payload, payloadLen);

		pos += frameLen;
	}

	// Remove consumed bytes from the buffer
	if (pos > 0)
		m_buffer.remove(0, pos);
}


std::pair<std::uint8_t, std::uint8_t> UbxParser::fletcher8(const char* data, int length)
{
	std::uint8_t ckA = 0;
	std::uint8_t ckB = 0;
	for (int i = 0; i < length; ++i)
	{
		ckA += static_cast<std::uint8_t>(data[i]);
		ckB += ckA;
	}
	return {ckA, ckB};
}


void UbxParser::dispatchMessage(std::uint8_t msgClass, std::uint8_t msgId,
                                const char* payload, int length)
{
	if (msgClass == Ubx::kClassNAV)
	{
		switch (msgId) {
		case Ubx::kIdNAV_PVT:    handleNavPvt(payload, length);    return;
		case Ubx::kIdNAV_DOP:    handleNavDop(payload, length);    return;
		case Ubx::kIdNAV_SAT:    handleNavSat(payload, length);    return;
		case Ubx::kIdNAV_COV:    handleNavCov(payload, length);    return;
		case Ubx::kIdNAV_STATUS: handleNavStatus(payload, length); return;
		default: break;
		}
	}
	else if (msgClass == Ubx::kClassMON)
	{
		switch (msgId) {
		case Ubx::kIdMON_VER: handleMonVer(payload, length); return;
		default: break;
		}
	}

	++m_stats.unknownMessages;
}


GnssFixType UbxParser::classifyFix(std::uint8_t fixType, std::uint8_t flags)
{
	// Check gnssFixOK first — if not set, the fix is not valid
	if ((flags & 0x01) == 0)
		return GnssFixType::NoFix;

	// Check carrier solution status (bits 7..6 of flags)
	int carrSoln = (flags >> 6) & 0x03;
	if (carrSoln == 2)
		return GnssFixType::RtkFixed;
	if (carrSoln == 1)
		return GnssFixType::RtkFloat;

	// Check differential solution (bit 1 of flags)
	if ((flags & 0x02) != 0)
		return GnssFixType::DGPS;

	// Standard fix classification
	switch (fixType) {
	case 2:  return GnssFixType::Fix2D;
	case 3:  return GnssFixType::Fix3D;
	case 4:  return GnssFixType::Fix3D;  // GNSS+dead reckoning → treat as 3D
	default: return GnssFixType::NoFix;
	}
}


// ---- Message handlers ----


void UbxParser::handleNavPvt(const char* payload, int length)
{
	// NAV-PVT is 92 bytes for all known protocol versions.
	// We accept payloads >= 92 to handle future extensions gracefully.
	if (length < static_cast<int>(sizeof(Ubx::NavPvt)))
		return;

	Ubx::NavPvt pvt;
	std::memcpy(&pvt, payload, sizeof(pvt));

	GnssPositionObservation observation;
	observation.meta.source = GnssObservationSource::UbxNavPvt;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.meta.timestampHasDate = true;
	observation.meta.timestampHasTime = true;
	observation.meta.limitation = QStringLiteral("u-blox accuracy values are receiver-estimated and treated as ~1-sigma");
	auto& pos = observation.position;
	pos.fixType = classifyFix(pvt.fixType, pvt.flags);
	pos.valid = (pos.fixType != GnssFixType::NoFix);

	pos.latitude  = pvt.latitudeDeg();
	pos.longitude = pvt.longitudeDeg();
	pos.altitude  = pvt.heightM();
	pos.altitudeMsl = pvt.heightMslM();
	pos.geoidSeparation = pos.altitude - pos.altitudeMsl;

	pos.hAccuracy = pvt.hAccuracyM();
	pos.vAccuracy = pvt.vAccuracyM();
	pos.accuracyBasis = GnssAccuracyBasis::Sigma68;  // u-blox reports ~1-sigma
	pos.computeP95();

	pos.pDOP = pvt.pDopScaled();
	pos.satellitesUsed = pvt.numSV;

	pos.groundSpeed = pvt.groundSpeedMs();
	pos.headingMotion = pvt.headingMotionDeg();
	pos.speedAccuracy = pvt.speedAccuracyMs();

	// Construct UTC timestamp from the date/time fields
	if (pvt.validDate() && pvt.validTime())
	{
		QDate date(pvt.year, pvt.month, pvt.day);
		QTime time(pvt.hour, pvt.min, pvt.sec);
		if (date.isValid() && time.isValid())
		{
			pos.timestamp = QDateTime(date, time, QTimeZone::UTC);
			// Add nanosecond fraction if meaningful
			if (pvt.nano != 0)
				pos.timestamp = pos.timestamp.addMSecs(pvt.nano / 1000000);
		}
	}

	emit positionObservation(observation);
}


void UbxParser::handleNavDop(const char* payload, int length)
{
	if (length < static_cast<int>(sizeof(Ubx::NavDop)))
		return;

	Ubx::NavDop dop;
	std::memcpy(&dop, payload, sizeof(dop));

	GnssDopObservation observation;
	observation.meta.source = GnssObservationSource::UbxNavDop;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.gDOP = dop.gDopScaled();
	observation.pDOP = dop.pDopScaled();
	observation.tDOP = dop.tDopScaled();
	observation.vDOP = dop.vDopScaled();
	observation.hDOP = dop.hDopScaled();
	observation.nDOP = dop.nDopScaled();
	observation.eDOP = dop.eDopScaled();
	emit dopObservation(observation);
}


void UbxParser::handleNavSat(const char* payload, int length)
{
	if (length < static_cast<int>(sizeof(Ubx::NavSatHeader)))
		return;

	Ubx::NavSatHeader header;
	std::memcpy(&header, payload, sizeof(header));

	int expectedLen = static_cast<int>(sizeof(Ubx::NavSatHeader))
	                  + header.numSvs * static_cast<int>(sizeof(Ubx::NavSatEntry));
	if (length < expectedLen)
		return;

	int totalUsed = 0;
	int totalVisible = header.numSvs;

	const char* entryPtr = payload + sizeof(Ubx::NavSatHeader);
	for (int i = 0; i < header.numSvs; ++i)
	{
		Ubx::NavSatEntry entry;
		std::memcpy(&entry, entryPtr + i * sizeof(Ubx::NavSatEntry), sizeof(entry));
		if (entry.svUsed())
			++totalUsed;
	}

	GnssSatelliteObservation observation;
	observation.meta.source = GnssObservationSource::UbxNavSat;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.satellitesUsed = totalUsed;
	observation.satellitesVisible = totalVisible;
	emit satelliteObservation(observation);
}


void UbxParser::handleNavCov(const char* payload, int length)
{
	if (length < static_cast<int>(sizeof(Ubx::NavCov)))
		return;

	Ubx::NavCov cov;
	std::memcpy(&cov, payload, sizeof(cov));

	if (!cov.posCovValid)
		return;

	// Emit the horizontal (NE) covariance for P95 ellipse computation.
	// The position covariance is in NED frame.
	GnssCovarianceObservation observation;
	observation.meta.source = GnssObservationSource::UbxNavCov;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.covNN = cov.posCovNN;
	observation.covNE = cov.posCovNE;
	observation.covEE = cov.posCovEE;
	emit covarianceObservation(observation);
}


void UbxParser::handleNavStatus(const char* payload, int length)
{
	if (length < static_cast<int>(sizeof(Ubx::NavStatus)))
		return;

	Ubx::NavStatus status;
	std::memcpy(&status, payload, sizeof(status));

	GnssStatusObservation observation;
	observation.meta.source = GnssObservationSource::UbxNavStatus;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.fixOK = status.gpsFixOK();
	observation.diffSoln = status.diffSoln();
	observation.carrSoln = status.carrSoln();
	observation.spoofDet = status.spoofDet();
	emit statusObservation(observation);
}


void UbxParser::handleMonVer(const char* payload, int length)
{
	if (length < static_cast<int>(sizeof(Ubx::MonVerHeader)))
		return;

	Ubx::MonVerHeader header;
	std::memcpy(&header, payload, sizeof(header));

	auto sw = QString::fromLatin1(header.swVersion, qstrnlen(header.swVersion, 30));
	auto hw = QString::fromLatin1(header.hwVersion, qstrnlen(header.hwVersion, 10));

	QStringList extensions;
	int extOffset = sizeof(Ubx::MonVerHeader);
	while (extOffset + Ubx::kMonVerExtensionSize <= length)
	{
		const char* ext = payload + extOffset;
		auto str = QString::fromLatin1(ext, qstrnlen(ext, Ubx::kMonVerExtensionSize));
		if (!str.isEmpty())
			extensions.append(str);
		extOffset += Ubx::kMonVerExtensionSize;
	}

	GnssVersionObservation observation;
	observation.meta.source = GnssObservationSource::UbxMonVer;
	observation.meta.observedAt = QDateTime::currentDateTimeUtc();
	observation.swVersion = sw;
	observation.hwVersion = hw;
	observation.extensions = extensions;
	emit versionObservation(observation);
}


}  // namespace OpenOrienteering
