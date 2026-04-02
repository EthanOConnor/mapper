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


#ifndef OPENORIENTEERING_UBX_CONFIG_H
#define OPENORIENTEERING_UBX_CONFIG_H

#include <cstdint>
#include <utility>

#include <QByteArray>
#include <QVector>

namespace OpenOrienteering {

/// Builds UBX protocol commands for configuring u-blox GNSS receivers.
///
/// Supports:
///   - Raw UBX frame construction with Fletcher-8 checksum
///   - UBX-CFG-VALSET for F9/X20 configuration (protocol >= 27)
///   - UBX-CFG-MSG for legacy message rate configuration
///   - Standard receiver initialization sequence
class UbxConfig
{
public:
	// UBX class/ID constants
	static constexpr std::uint8_t kClassCfg = 0x06;
	static constexpr std::uint8_t kIdCfgValset = 0x8A;
	static constexpr std::uint8_t kIdCfgMsg = 0x01;
	static constexpr std::uint8_t kIdCfgRate = 0x08;
	static constexpr std::uint8_t kIdCfgPrt = 0x00;

	// Message class/ID for the messages we want to enable
	static constexpr std::uint8_t kClassNav = 0x01;
	static constexpr std::uint8_t kClassMon = 0x0A;
	static constexpr std::uint8_t kIdNavPvt = 0x07;
	static constexpr std::uint8_t kIdNavDop = 0x04;
	static constexpr std::uint8_t kIdNavSat = 0x35;
	static constexpr std::uint8_t kIdNavCov = 0x36;
	static constexpr std::uint8_t kIdNavStatus = 0x03;
	static constexpr std::uint8_t kIdMonVer = 0x04;

	/// Build a raw UBX frame from class, id, and payload.
	static QByteArray buildFrame(std::uint8_t msgClass, std::uint8_t msgId,
	                             const QByteArray& payload);

	/// Build a UBX-CFG-MSG command to set the output rate for a message.
	/// rate = messages per navigation solution (0 = disabled, 1 = every epoch).
	static QByteArray buildCfgMsg(std::uint8_t msgClass, std::uint8_t msgId,
	                              std::uint8_t rate);

	/// Build a UBX-CFG-RATE command to set the navigation solution rate.
	/// measRateMs: measurement period in ms (e.g., 100 for 10 Hz, 1000 for 1 Hz).
	static QByteArray buildCfgRate(std::uint16_t measRateMs,
	                               std::uint16_t navRate = 1);

	/// Build a UBX-CFG-VALSET command (for F9/X20 receivers, protocol >= 27).
	/// Keys are 32-bit config key IDs, values are the raw bytes.
	struct CfgKeyValue {
		std::uint32_t key;
		QByteArray value;  // 1, 2, 4, or 8 bytes depending on key size
	};
	static QByteArray buildCfgValset(const QVector<CfgKeyValue>& items,
	                                 std::uint8_t layer = 1);  // 1 = RAM

	/// Build the standard initialization command sequence.
	/// Returns a list of UBX frames to send sequentially.
	/// Enables NAV-PVT, NAV-DOP, NAV-SAT, NAV-COV, NAV-STATUS at 1 Hz,
	/// and requests MON-VER once.
	static QVector<QByteArray> buildInitSequence();

	/// Build a poll request for a specific message (empty payload = poll).
	static QByteArray buildPollRequest(std::uint8_t msgClass, std::uint8_t msgId);

private:
	/// Compute Fletcher-8 checksum over bytes [data, data+length).
	static std::pair<std::uint8_t, std::uint8_t> fletcher8(
	    const char* data, int length);
};

}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_UBX_CONFIG_H
