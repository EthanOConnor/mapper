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


#ifndef OPENORIENTEERING_NMEA_PARSER_H
#define OPENORIENTEERING_NMEA_PARSER_H

#include <cstdint>

#include <QByteArray>
#include <QObject>

#include "gnss/gnss_observation.h"

namespace OpenOrienteering {


/// Incremental NMEA 0183 sentence parser.
///
/// Feed raw bytes via addData(). The parser extracts complete NMEA sentences
/// (delimited by CR/LF), validates checksums via minmea, and emits signals
/// for decoded position and quality data.
///
/// Supported sentence types:
///   - GGA: fix data, altitude, accuracy (via HDOP), correction age
///   - RMC: position, speed, course, date/time
///   - GSA: DOP values, fix mode
///   - GSV: satellites in view
///
/// NMEA is the fallback protocol for non-u-blox receivers. When used with
/// u-blox receivers, prefer UBX for richer metadata.
class NmeaParser : public QObject
{
	Q_OBJECT

public:
	explicit NmeaParser(QObject* parent = nullptr);
	~NmeaParser() override;

	/// Feed raw bytes. May emit zero or more signals.
	void addData(const QByteArray& data);

	/// Reset parser state.
	void reset();

	struct Stats
	{
		std::uint64_t sentencesParsed = 0;
		std::uint64_t checksumErrors  = 0;
		std::uint64_t bytesProcessed  = 0;
	};

	const Stats& stats() const { return m_stats; }

signals:
	/// Emitted when a GGA or RMC sentence provides an observation.
	void positionObservation(const OpenOrienteering::GnssPositionObservation& observation);

	/// Emitted when GSA provides DOP values.
	void dopObservation(const OpenOrienteering::GnssDopObservation& observation);

	/// Emitted when GSV provides satellite count.
	void satelliteObservation(const OpenOrienteering::GnssSatelliteObservation& observation);

private:
	/// Process a single complete NMEA sentence (including $ and checksum).
	void processSentence(const QByteArray& sentence);

	void handleGGA(const char* sentence);
	void handleRMC(const char* sentence);
	void handleGSA(const char* sentence);
	void handleGSV(const char* sentence);

	QByteArray m_lineBuffer;
	Stats m_stats;

	static constexpr int kMaxLineLength = 256;  // NMEA max is 82, generous buffer
};


}  // namespace OpenOrienteering

#endif
