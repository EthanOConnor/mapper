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


#ifndef OPENORIENTEERING_GGA_GENERATOR_H
#define OPENORIENTEERING_GGA_GENERATOR_H

#include <QByteArray>

namespace OpenOrienteering {

struct GnssPosition;


/// Generates NMEA 0183 GGA sentences from GNSS position data.
///
/// Used to provide position feedback to VRS NTRIP casters, which need
/// the rover's approximate location to compute virtual reference station
/// corrections.
namespace GgaGenerator {

/// Generate a $GPGGA sentence from a GnssPosition.
/// Returns the complete sentence including leading '$' and trailing checksum + CRLF.
/// If the position is invalid, returns a GGA with no-fix indicator (quality=0).
QByteArray fromPosition(const GnssPosition& pos);

/// Generate a GGA sentence from raw coordinates (for bootstrap before first fix).
/// quality: 0=invalid, 1=GPS, 2=DGPS, 4=RTK fixed, 5=RTK float
QByteArray fromCoordinates(double latitude, double longitude,
                           double altitudeMsl = 0.0,
                           int quality = 1, int numSatellites = 0,
                           float hdop = 99.9f,
                           float geoidSep = 0.0f);

}  // namespace GgaGenerator


}  // namespace OpenOrienteering

#endif
