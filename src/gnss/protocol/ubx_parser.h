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


#ifndef OPENORIENTEERING_UBX_PARSER_H
#define OPENORIENTEERING_UBX_PARSER_H

#include <cstdint>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include "gnss/gnss_observation.h"

namespace OpenOrienteering {

namespace Ubx { struct NavPvt; struct NavDop; struct NavSatHeader; struct NavSatEntry; struct NavCov; struct NavStatus; struct MonVerHeader; }


/// Incremental parser for the u-blox UBX binary protocol.
///
/// Feed raw bytes via addData(). The parser maintains an internal buffer,
/// scans for UBX sync bytes (0xB5 0x62), validates frame checksums, and
/// emits signals for each decoded message.
///
/// Handles:
///   - Byte-level resynchronization (recovers from partial frames, noise)
///   - Variable-length messages (NAV-SAT, MON-VER)
///   - Cross-generation compatibility (F9P protocol 27, X20P protocol 34)
///
/// The parser does NOT interpret non-UBX bytes (NMEA sentences, RTCM frames)
/// in the stream. Those are silently skipped during sync-byte scanning.
class UbxParser : public QObject
{
	Q_OBJECT

public:
	explicit UbxParser(QObject* parent = nullptr);
	~UbxParser() override;

	/// Feed raw bytes from the transport layer.
	/// May emit zero or more signals per call.
	void addData(const QByteArray& data);

	/// Reset parser state and discard buffered data.
	void reset();

	/// Statistics for diagnostics.
	struct Stats
	{
		std::uint64_t framesDecoded    = 0;
		std::uint64_t checksumErrors   = 0;
		std::uint64_t unknownMessages  = 0;
		std::uint64_t bytesProcessed   = 0;
		std::uint64_t syncResyncCount  = 0;
	};

	const Stats& stats() const { return m_stats; }

signals:
	/// Emitted for each successfully decoded NAV-PVT message.
	void positionObservation(const OpenOrienteering::GnssPositionObservation& observation);

	/// Emitted for NAV-DOP with all DOP values.
	void dopObservation(const OpenOrienteering::GnssDopObservation& observation);

	/// Emitted for NAV-SAT with per-satellite info.
	void satelliteObservation(const OpenOrienteering::GnssSatelliteObservation& observation);

	/// Emitted when NAV-COV provides a valid position covariance.
	void covarianceObservation(const OpenOrienteering::GnssCovarianceObservation& observation);

	/// Emitted for NAV-STATUS with fix status details.
	void statusObservation(const OpenOrienteering::GnssStatusObservation& observation);

	/// Emitted when MON-VER is decoded.
	void versionObservation(const OpenOrienteering::GnssVersionObservation& observation);

	/// Emitted for any valid UBX frame (class, id, raw payload).
	/// Useful for logging and passthrough of unhandled messages.
	void rawFrame(std::uint8_t msgClass, std::uint8_t msgId, const QByteArray& payload);

private:
	/// Attempt to extract and process complete UBX frames from the buffer.
	void processBuffer();

	/// Dispatch a validated frame payload to the appropriate handler.
	void dispatchMessage(std::uint8_t msgClass, std::uint8_t msgId,
	                     const char* payload, int length);

	// Message handlers
	void handleNavPvt(const char* payload, int length);
	void handleNavDop(const char* payload, int length);
	void handleNavSat(const char* payload, int length);
	void handleNavCov(const char* payload, int length);
	void handleNavStatus(const char* payload, int length);
	void handleMonVer(const char* payload, int length);

	/// Compute Fletcher-8 checksum over a range of bytes.
	/// Returns {ck_a, ck_b}.
	static std::pair<std::uint8_t, std::uint8_t> fletcher8(const char* data, int length);

	/// Convert UBX fixType + flags to our GnssFixType enum.
	static GnssFixType classifyFix(std::uint8_t fixType, std::uint8_t flags);

	QByteArray m_buffer;
	Stats m_stats;

	/// Maximum buffer size before we force a trim (prevent unbounded growth
	/// if the stream contains no valid UBX data).
	static constexpr int kMaxBufferSize = 16384;
};


}  // namespace OpenOrienteering

#endif
