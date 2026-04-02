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


#include "ubx_config.h"

namespace OpenOrienteering {

std::pair<std::uint8_t, std::uint8_t> UbxConfig::fletcher8(
    const char* data, int length)
{
	std::uint8_t ck_a = 0;
	std::uint8_t ck_b = 0;
	for (int i = 0; i < length; ++i)
	{
		ck_a += static_cast<std::uint8_t>(data[i]);
		ck_b += ck_a;
	}
	return {ck_a, ck_b};
}


QByteArray UbxConfig::buildFrame(std::uint8_t msgClass, std::uint8_t msgId,
                                 const QByteArray& payload)
{
	const auto len = static_cast<std::uint16_t>(payload.size());

	// Total frame: sync(2) + class(1) + id(1) + length(2) + payload(N) + ck(2)
	QByteArray frame;
	frame.reserve(8 + payload.size());

	// Sync bytes
	frame.append(static_cast<char>(0xB5));
	frame.append(static_cast<char>(0x62));

	// Class and ID
	frame.append(static_cast<char>(msgClass));
	frame.append(static_cast<char>(msgId));

	// Length (little-endian)
	frame.append(static_cast<char>(len & 0xFF));
	frame.append(static_cast<char>((len >> 8) & 0xFF));

	// Payload
	frame.append(payload);

	// Checksum over class + id + length + payload (bytes 2 through end)
	auto [ck_a, ck_b] = fletcher8(frame.constData() + 2, frame.size() - 2);
	frame.append(static_cast<char>(ck_a));
	frame.append(static_cast<char>(ck_b));

	return frame;
}


QByteArray UbxConfig::buildCfgMsg(std::uint8_t msgClass, std::uint8_t msgId,
                                  std::uint8_t rate)
{
	QByteArray payload;
	payload.append(static_cast<char>(msgClass));
	payload.append(static_cast<char>(msgId));
	payload.append(static_cast<char>(rate));
	return buildFrame(kClassCfg, kIdCfgMsg, payload);
}


QByteArray UbxConfig::buildCfgRate(std::uint16_t measRateMs,
                                   std::uint16_t navRate)
{
	QByteArray payload;
	payload.reserve(6);

	// measRate (little-endian)
	payload.append(static_cast<char>(measRateMs & 0xFF));
	payload.append(static_cast<char>((measRateMs >> 8) & 0xFF));

	// navRate (little-endian)
	payload.append(static_cast<char>(navRate & 0xFF));
	payload.append(static_cast<char>((navRate >> 8) & 0xFF));

	// timeRef: 1 = GPS time
	payload.append(static_cast<char>(0x01));
	payload.append(static_cast<char>(0x00));

	return buildFrame(kClassCfg, kIdCfgRate, payload);
}


QByteArray UbxConfig::buildCfgValset(const QVector<CfgKeyValue>& items,
                                     std::uint8_t layer)
{
	QByteArray payload;

	// Version
	payload.append(static_cast<char>(0x00));

	// Layers
	payload.append(static_cast<char>(layer));

	// Reserved (2 bytes)
	payload.append(static_cast<char>(0x00));
	payload.append(static_cast<char>(0x00));

	// Key-value pairs
	for (const auto& item : items)
	{
		// Key (4 bytes, little-endian)
		payload.append(static_cast<char>(item.key & 0xFF));
		payload.append(static_cast<char>((item.key >> 8) & 0xFF));
		payload.append(static_cast<char>((item.key >> 16) & 0xFF));
		payload.append(static_cast<char>((item.key >> 24) & 0xFF));

		// Value (variable length)
		payload.append(item.value);
	}

	return buildFrame(kClassCfg, kIdCfgValset, payload);
}


QByteArray UbxConfig::buildPollRequest(std::uint8_t msgClass, std::uint8_t msgId)
{
	return buildFrame(msgClass, msgId, QByteArray());
}


QVector<QByteArray> UbxConfig::buildInitSequence()
{
	QVector<QByteArray> sequence;
	sequence.reserve(6);

	// Enable NAV-PVT at every epoch
	sequence.append(buildCfgMsg(kClassNav, kIdNavPvt, 1));

	// Enable NAV-DOP at every epoch
	sequence.append(buildCfgMsg(kClassNav, kIdNavDop, 1));

	// Enable NAV-SAT every 5th epoch (reduce bandwidth)
	sequence.append(buildCfgMsg(kClassNav, kIdNavSat, 5));

	// Enable NAV-COV at every epoch
	sequence.append(buildCfgMsg(kClassNav, kIdNavCov, 1));

	// Enable NAV-STATUS at every epoch
	sequence.append(buildCfgMsg(kClassNav, kIdNavStatus, 1));

	// Poll MON-VER once
	sequence.append(buildPollRequest(kClassMon, kIdMonVer));

	return sequence;
}

}  // namespace OpenOrienteering
