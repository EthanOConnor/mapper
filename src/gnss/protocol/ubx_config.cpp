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

#if defined(MAPPER_GNSS_USE_GLEAN)
#  include <array>
#  include <glean/ubx.h>
#endif

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
#if defined(MAPPER_GNSS_USE_GLEAN)
	if (items.size() > 64)
		return {};

	std::array<glean_ubx_cfg_item_t, 64> gleanItems = {};
	for (qsizetype i = 0; i < items.size(); ++i)
	{
		const auto& source = items[i];
		auto& target = gleanItems[static_cast<std::size_t>(i)];
		target.key = source.key;
		auto byte = [&source](qsizetype offset) {
			return static_cast<std::uint8_t>(source.value[offset]);
		};
		switch ((source.key >> 28) & 0x07u)
		{
		case 1:
			if (source.value.size() != 1) return {};
			target.type = GLEAN_UBX_CFG_L;
			target.value.l = byte(0) != 0;
			break;
		case 2:
			if (source.value.size() != 1) return {};
			target.type = GLEAN_UBX_CFG_U1;
			target.value.u1 = byte(0);
			break;
		case 3:
			if (source.value.size() != 2) return {};
			target.type = GLEAN_UBX_CFG_U2;
			target.value.u2 = static_cast<std::uint16_t>(byte(0))
			                | static_cast<std::uint16_t>(byte(1)) << 8;
			break;
		case 4:
			if (source.value.size() != 4) return {};
			target.type = GLEAN_UBX_CFG_U4;
			target.value.u4 = static_cast<std::uint32_t>(byte(0))
			                | static_cast<std::uint32_t>(byte(1)) << 8
			                | static_cast<std::uint32_t>(byte(2)) << 16
			                | static_cast<std::uint32_t>(byte(3)) << 24;
			break;
		case 5:
			if (source.value.size() != 8) return {};
			target.type = GLEAN_UBX_CFG_U8;
			target.value.u8 = 0;
			for (int shift = 0; shift < 8; ++shift)
				target.value.u8 |= static_cast<std::uint64_t>(byte(shift))
				                 << (shift * 8);
			break;
		default:
			return {};
		}
	}

	std::array<std::uint8_t, 1024> output = {};
	std::size_t outputSize = 0;
	auto result = glean_ubx_encode_cfg_valset(
	  layer, gleanItems.data(), static_cast<std::size_t>(items.size()),
	  output.data(), output.size(), &outputSize);
	if (result != GLEAN_OK)
		return {};
	return QByteArray(reinterpret_cast<const char*>(output.data()),
	                  static_cast<qsizetype>(outputSize));
#else
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
#endif
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
#if defined(MAPPER_GNSS_USE_GLEAN)
	// Glean owns the validated u-blox configuration-key catalog. Keep the
	// fallback literals below compile-time checked against that source of truth.
	static_assert(0x10740001u == GLEAN_UBX_KEY_CFG_UART1OUTPROT_UBX);
	static_assert(0x10760001u == GLEAN_UBX_KEY_CFG_UART2OUTPROT_UBX);
	static_assert(0x20910007u == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_PVT_UART1);
	static_assert(0x20910008u == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_PVT_UART2);
	static_assert(0x20910034u == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_HPPOSLLH_UART1);
	static_assert(0x20910035u == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_HPPOSLLH_UART2);
	static_assert(0x20910084u == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_COV_UART1);
	static_assert(0x20910085u == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_COV_UART2);
	static_assert(0x20910039u == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_DOP_UART1);
	static_assert(0x2091003au == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_DOP_UART2);
	static_assert(0x2091001bu == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_STATUS_UART1);
	static_assert(0x2091001cu == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_STATUS_UART2);
	static_assert(0x20910016u == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_SAT_UART1);
	static_assert(0x20910017u == GLEAN_UBX_KEY_CFG_MSGOUT_UBX_NAV_SAT_UART2);
	static_assert(0x10730004u == GLEAN_UBX_KEY_CFG_UART1INPROT_RTCM3X);
	static_assert(0x10750004u == GLEAN_UBX_KEY_CFG_UART2INPROT_RTCM3X);
	static_assert(0x10730001u == GLEAN_UBX_KEY_CFG_UART1INPROT_UBX);
	static_assert(0x10750001u == GLEAN_UBX_KEY_CFG_UART2INPROT_UBX);
#endif
	QVector<QByteArray> sequence;

	// Use CFG-VALSET (modern key-value config, works on F9P and X20P).
	// Configure both receiver UARTs. ArduSimple-compatible NUS bridges can be
	// wired to either UART through the XBee socket, and u-blox defaults leave
	// UART2's navigation output disabled. Configuring UART1 alone therefore
	// produces a deceptively healthy BLE/NTRIP connection with no position
	// stream when the bridge is on UART2. The changes are RAM-only and leave
	// the receiver's existing NMEA settings intact.
	//
	// Config keys from u-blox F9 HPG 1.51 / X20 HPG 2.02 interface descriptions.
	// Layer = 0x01 (RAM only, self-healing on power cycle).

	QVector<CfgKeyValue> items;
	items.reserve(20);

	// --- Enable UBX output on both possible bridge UARTs ---
	items.append({0x10740001, u1(1)});  // CFG-UART1OUTPROT-UBX = true
	items.append({0x10760001, u1(1)});  // CFG-UART2OUTPROT-UBX = true

	// --- Primary messages: every epoch on UART1 and UART2 ---
	items.append({0x20910007, u1(1)});  // CFG-MSGOUT-UBX_NAV_PVT_UART1 = 1
	items.append({0x20910008, u1(1)});  // CFG-MSGOUT-UBX_NAV_PVT_UART2 = 1
	items.append({0x20910034, u1(1)});  // CFG-MSGOUT-UBX_NAV_HPPOSLLH_UART1 = 1
	items.append({0x20910035, u1(1)});  // CFG-MSGOUT-UBX_NAV_HPPOSLLH_UART2 = 1
	items.append({0x20910084, u1(1)});  // CFG-MSGOUT-UBX_NAV_COV_UART1 = 1
	items.append({0x20910085, u1(1)});  // CFG-MSGOUT-UBX_NAV_COV_UART2 = 1
	items.append({0x20910039, u1(1)});  // CFG-MSGOUT-UBX_NAV_DOP_UART1 = 1
	items.append({0x2091003a, u1(1)});  // CFG-MSGOUT-UBX_NAV_DOP_UART2 = 1
	items.append({0x2091001b, u1(1)});  // CFG-MSGOUT-UBX_NAV_STATUS_UART1 = 1
	items.append({0x2091001c, u1(1)});  // CFG-MSGOUT-UBX_NAV_STATUS_UART2 = 1

	// --- Diagnostic messages: every 5th epoch on both UARTs ---
	items.append({0x20910016, u1(5)});  // CFG-MSGOUT-UBX_NAV_SAT_UART1 = 5
	items.append({0x20910017, u1(5)});  // CFG-MSGOUT-UBX_NAV_SAT_UART2 = 5

	// --- Accept RTCM3 corrections and UBX commands on either bridge UART ---
	items.append({0x10730004, u1(1)});  // CFG-UART1INPROT-RTCM3X = true
	items.append({0x10750004, u1(1)});  // CFG-UART2INPROT-RTCM3X = true
	items.append({0x10730001, u1(1)});  // CFG-UART1INPROT-UBX = true
	items.append({0x10750001, u1(1)});  // CFG-UART2INPROT-UBX = true

	sequence.append(buildCfgValset(items, 0x01));  // RAM layer

	// Poll MON-VER once to identify receiver model
	sequence.append(buildPollRequest(kClassMon, kIdMonVer));

	return sequence;
}

}  // namespace OpenOrienteering
