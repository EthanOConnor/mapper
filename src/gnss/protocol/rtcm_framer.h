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


#ifndef OPENORIENTEERING_RTCM_FRAMER_H
#define OPENORIENTEERING_RTCM_FRAMER_H

#include <cstdint>

#include <QByteArray>
#include <QObject>

namespace OpenOrienteering {


/// RTCM 3.x frame validator for outbound correction streams.
///
/// This does NOT decode RTCM message content. It validates frame structure
/// (preamble, length, CRC-24Q) to monitor correction stream health. Use it
/// to track whether the NTRIP correction data is well-formed and flowing.
///
/// RTCM 3.x frame format:
///   Byte 0:     Preamble (0xD3)
///   Bytes 1-2:  6 reserved bits + 10-bit message length
///   Bytes 3..N: Message data (N = length)
///   Last 3:     CRC-24Q
///
/// Total frame size: 3 (header) + length + 3 (CRC) = length + 6
class RtcmFramer : public QObject
{
	Q_OBJECT

public:
	explicit RtcmFramer(QObject* parent = nullptr);
	~RtcmFramer() override;

	/// Feed raw correction bytes. Validates frames and emits signals.
	void addData(const QByteArray& data);

	/// Reset state and discard buffered data.
	void reset();

	struct Stats
	{
		std::uint64_t framesValidated  = 0;
		std::uint64_t crcErrors        = 0;
		std::uint64_t bytesProcessed   = 0;
	};

	const Stats& stats() const { return m_stats; }

signals:
	/// Emitted for each valid RTCM frame. Reports the 12-bit message type.
	void frameValidated(int messageType, int payloadLength);

	/// Emitted when a CRC error is detected.
	void crcError();

private:
	void processBuffer();

	/// Compute CRC-24Q over a range of bytes.
	/// RTCM 3.x uses the Qualcomm CRC-24Q polynomial: 0x1864CFB.
	static std::uint32_t crc24q(const char* data, int length);

	QByteArray m_buffer;
	Stats m_stats;

	/// RTCM preamble byte.
	static constexpr std::uint8_t kPreamble = 0xD3;
	/// Maximum RTCM 3.x message length (10-bit field).
	static constexpr int kMaxMessageLength = 1023;
	/// Maximum buffer before trim.
	static constexpr int kMaxBufferSize = 8192;
};


}  // namespace OpenOrienteering

#endif
