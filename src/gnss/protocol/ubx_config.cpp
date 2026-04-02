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


static QByteArray u1(std::uint8_t v)
{
	return QByteArray(1, static_cast<char>(v));
}

QVector<QByteArray> UbxConfig::buildInitSequence()
{
	QVector<QByteArray> sequence;

	// Use CFG-VALSET (modern key-value config, works on F9P and X20P).
	// Target UART1 explicitly — this is the port the ArduSimple BLE bridge
	// connects to. Legacy CFG-MSG sets the "current port" which may not be
	// UART1 when commands arrive via BLE.
	//
	// Config keys from u-blox F9 HPG 1.51 / X20 HPG 2.02 interface descriptions.
	// Layer = 0x01 (RAM only, self-healing on power cycle).

	QVector<CfgKeyValue> items;
	items.reserve(16);

	// --- Enable UBX output on UART1 ---
	items.append({0x10740001, u1(1)});  // CFG-UART1OUTPROT-UBX = true

	// --- Primary messages: every epoch on UART1 ---
	items.append({0x20910007, u1(1)});  // CFG-MSGOUT-UBX_NAV_PVT_UART1 = 1
	items.append({0x20910034, u1(1)});  // CFG-MSGOUT-UBX_NAV_HPPOSLLH_UART1 = 1
	items.append({0x20910085, u1(1)});  // CFG-MSGOUT-UBX_NAV_COV_UART1 = 1
	items.append({0x20910039, u1(1)});  // CFG-MSGOUT-UBX_NAV_DOP_UART1 = 1
	items.append({0x2091001b, u1(1)});  // CFG-MSGOUT-UBX_NAV_STATUS_UART1 = 1

	// --- Diagnostic messages: every 5th epoch on UART1 ---
	items.append({0x20910016, u1(5)});  // CFG-MSGOUT-UBX_NAV_SAT_UART1 = 5

	// --- Accept RTCM3 corrections on UART1 (BLE bridge forwards these) ---
	items.append({0x10730004, u1(1)});  // CFG-UART1INPROT-RTCM3X = true

	// --- Accept UBX commands on UART1 ---
	items.append({0x10730001, u1(1)});  // CFG-UART1INPROT-UBX = true

	sequence.append(buildCfgValset(items, 0x01));  // RAM layer

	// Poll MON-VER once to identify receiver model
	sequence.append(buildPollRequest(kClassMon, kIdMonVer));

	return sequence;
}

}  // namespace OpenOrienteering
