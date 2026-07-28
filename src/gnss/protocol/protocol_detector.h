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


#ifndef OPENORIENTEERING_PROTOCOL_DETECTOR_H
#define OPENORIENTEERING_PROTOCOL_DETECTOR_H

#include <QByteArray>

#include "gnss/gnss_state.h"

namespace OpenOrienteering {


/// Detects whether an incoming byte stream contains UBX, NMEA, or both.
///
/// Feed the first few hundred bytes of a GNSS receiver stream to detect()
/// and it will classify the protocol. u-blox receivers typically output
/// both UBX and NMEA by default, so Mixed is a common result.
///
/// This does not validate full frames — it only looks for characteristic
/// byte patterns to make a quick classification decision.
class ProtocolDetector
{
public:
	/// Analyze a buffer and return the detected protocol.
	/// Examines up to the first 512 bytes.
	static GnssProtocol detect(const QByteArray& data);

	/// Minimum bytes needed for a reliable detection.
	static constexpr int kMinDetectionBytes = 16;
};


}  // namespace OpenOrienteering

#endif
