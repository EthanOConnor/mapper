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


#ifndef OPENORIENTEERING_GNSS_STATE_H
#define OPENORIENTEERING_GNSS_STATE_H

#include <cstdint>

#include <QDateTime>
#include <QMetaType>
#include <QString>

#include "gnss_position.h"

namespace OpenOrienteering {


/// Transport-layer connection state.
enum class GnssTransportState : std::uint8_t
{
	Disconnected = 0,
	Connecting   = 1,
	Connected    = 2,
	Reconnecting = 3,
};


/// NTRIP correction stream health.
enum class GnssCorrectionState : std::uint8_t
{
	Disabled      = 0,  ///< No correction source configured
	Disconnected  = 1,  ///< Configured but not connected
	Connecting    = 2,
	Connected     = 3,  ///< TCP connected, waiting for data
	Flowing       = 4,  ///< RTCM bytes arriving
	Stale         = 5,  ///< No data received recently
	Reconnecting  = 6,
};


/// Which protocol the incoming GNSS data stream is using.
enum class GnssProtocol : std::uint8_t
{
	Unknown = 0,
	UBX     = 1,
	NMEA    = 2,
	Mixed   = 3,  ///< Both UBX and NMEA detected (common for u-blox defaults)
};


/// Satellite constellation identifier (matches UBX gnssId).
enum class GnssConstellation : std::uint8_t
{
	GPS     = 0,
	SBAS    = 1,
	Galileo = 2,
	BeiDou  = 3,
	IMES    = 4,
	QZSS    = 5,
	GLONASS = 6,
	NavIC   = 7,
};


/// Per-constellation satellite count.
struct GnssConstellationInfo
{
	std::uint8_t used    = 0;  ///< Satellites used in solution
	std::uint8_t visible = 0;  ///< Satellites in view
};


/// Full GNSS session state: position + transport + corrections + receiver info.
///
/// This composite captures everything the UI needs to display and the session
/// manager needs to track. Updated atomically by GnssSession.
struct GnssState
{
	// -- Position --
	GnssPosition position;

	// -- Transport --
	GnssTransportState transportState = GnssTransportState::Disconnected;
	QString deviceName;       ///< e.g. "u-blox ZED-F9P"
	QString transportType;    ///< e.g. "BLE", "TCP"

	// -- Protocol --
	GnssProtocol protocol = GnssProtocol::Unknown;

	// -- Corrections --
	GnssCorrectionState correctionState = GnssCorrectionState::Disabled;
	QString ntripMountpoint;
	QString ntripProfileName;
	float   correctionDataRate = 0.0f;  ///< bytes/sec, smoothed
	int     reconnectCount     = 0;

	// -- Reference frame (from NTRIP sourcetable or manual config) --
	QString referenceFrame;   ///< e.g. "ITRF2020", "ETRS89"

	// -- Receiver info (from UBX MON-VER) --
	QString receiverSwVersion;
	QString receiverHwVersion;
	QString receiverModel;    ///< Derived from MON-VER extension strings

	// -- Per-constellation satellite counts --
	static constexpr int kMaxConstellations = 8;
	GnssConstellationInfo constellations[kMaxConstellations] = {};

	// -- Session timing --
	QDateTime sessionStart;
	QDateTime lastPositionTime;
	QDateTime lastCorrectionTime;
};


}  // namespace OpenOrienteering

Q_DECLARE_METATYPE(OpenOrienteering::GnssState)

#endif
