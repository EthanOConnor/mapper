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

#include <cmath>
#include <cstring>
#include <utility>

#include <QTimeZone>

#include "ubx_messages.h"

namespace OpenOrienteering {


UbxParser::UbxParser(QObject* parent)
    : QObject(parent)
{
	m_buffer.reserve(4096);
}

UbxParser::~UbxParser() = default;


void UbxParser::addData(const QByteArray& data)
{
	if (data.isEmpty())
		return;

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
}


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
