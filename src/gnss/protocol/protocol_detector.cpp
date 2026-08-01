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

#include "protocol_detector.h"

#include <cstddef>
#include <cstdint>

#if defined(MAPPER_GNSS_USE_GLEAN_WIRE)
#  include <glean/wire.h>
#else
#include "protocol/ubx_messages.h"
#endif

namespace OpenOrienteering {


GnssProtocol ProtocolDetector::detect(const QByteArray& data)
{
	if (data.size() < kMinDetectionBytes)
		return GnssProtocol::Unknown;

#if defined(MAPPER_GNSS_USE_GLEAN_WIRE)
	glean_wire_scan_t scanner = {};
	glean_wire_scan_feed(
	  &scanner,
	  reinterpret_cast<const std::uint8_t*>(data.constData()),
	  static_cast<std::size_t>(qMin(data.size(), 4096)),
	  nullptr, nullptr);

	const bool hasUbx = glean_wire_scan_valid_count(
	  &scanner, GLEAN_WIRE_KIND_UBX) > 0;
	const bool hasNmea = glean_wire_scan_valid_count(
	  &scanner, GLEAN_WIRE_KIND_NMEA) > 0;
	if (hasUbx && hasNmea)
		return GnssProtocol::Mixed;
	if (hasUbx)
		return GnssProtocol::UBX;
	if (hasNmea)
		return GnssProtocol::NMEA;
	if (glean_wire_scan_valid_count(&scanner, GLEAN_WIRE_KIND_BYNAV) > 0)
		return GnssProtocol::BYNAV;
	if (glean_wire_scan_valid_count(&scanner, GLEAN_WIRE_KIND_BINEX) > 0)
		return GnssProtocol::BINEX;
	if (glean_wire_scan_valid_count(&scanner, GLEAN_WIRE_KIND_RTCM3) > 0)
		return GnssProtocol::RTCM3;
	return GnssProtocol::Unknown;
#else
	bool hasUbx = false;
	bool hasNmea = false;

	int limit = qMin(data.size(), 4096);

	for (int i = 0; i < limit; ++i)
	{
		auto byte = static_cast<std::uint8_t>(data[i]);

		// UBX sync: 0xB5 followed by 0x62
		if (byte == Ubx::kSyncChar1 && i + 1 < limit
		    && static_cast<std::uint8_t>(data[i + 1]) == Ubx::kSyncChar2)
		{
			hasUbx = true;
			if (hasNmea)
				return GnssProtocol::Mixed;
			++i;  // Skip second sync byte
			continue;
		}

		// NMEA: '$' followed by a valid talker ID character (uppercase letter)
		if (byte == '$' && i + 1 < limit)
		{
			auto next = static_cast<std::uint8_t>(data[i + 1]);
			if (next >= 'A' && next <= 'Z')
			{
				hasNmea = true;
				if (hasUbx)
					return GnssProtocol::Mixed;
			}
			continue;
		}

		// NMEA proprietary: '!' followed by uppercase
		if (byte == '!' && i + 1 < limit)
		{
			auto next = static_cast<std::uint8_t>(data[i + 1]);
			if (next >= 'A' && next <= 'Z')
			{
				hasNmea = true;
				if (hasUbx)
					return GnssProtocol::Mixed;
			}
		}
	}

	if (hasUbx)
		return GnssProtocol::UBX;
	if (hasNmea)
		return GnssProtocol::NMEA;
	return GnssProtocol::Unknown;
#endif
}


}  // namespace OpenOrienteering
