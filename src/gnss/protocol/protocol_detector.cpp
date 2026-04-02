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

#include <cstdint>

#include "protocol/ubx_messages.h"

namespace OpenOrienteering {


GnssProtocol ProtocolDetector::detect(const QByteArray& data)
{
	if (data.size() < kMinDetectionBytes)
		return GnssProtocol::Unknown;

	bool hasUbx = false;
	bool hasNmea = false;

	int limit = qMin(data.size(), 512);

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
}


}  // namespace OpenOrienteering
