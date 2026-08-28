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

#include <cmath>
#include <cstdint>

#include <QByteArray>
#include <QByteArrayList>
#include <QObject>
#include <QString>

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
///   - GST: receiver error statistics, paired to GGA by epoch
///
/// Quectel proprietary sentences (LC29H-class modules, including the HYFIX
/// GEO-PULSE) are decoded too:
///   - PQTMDRPVA: dead-reckoning position/velocity/attitude and solution type
///   - PQTMDRCAL: dead-reckoning calibration state and navigation type
///   - PQTMTXT:   receiver status text
///   - PAIR001:   acknowledgement of a $PAIR command
///
/// PQTMDRPVA never reports an RTK fix type, so it contributes velocity and
/// attitude rather than a primary position: taking its solution type as the
/// fix type would silently demote an RTK-fixed GGA to a plain 3D fix.
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

	/// Emitted when a Quectel PQTMDRPVA or PQTMDRCAL sentence reports
	/// dead-reckoning state.
	void deadReckoningObservation(const OpenOrienteering::GnssDeadReckoningObservation& observation);

	/// Emitted for each decoded proprietary sentence, by identifier
	/// (e.g. "PQTMDRPVA"). Standard sentences are already counted through
	/// their observations; these have no observation path of their own, and
	/// their presence is what shows whether the receiver is configured as
	/// expected. Used for message-rate statistics.
	void sentenceDecoded(const QString& identifier);

	/// Emitted for a receiver status text sentence ($PQTMTXT).
	void receiverStatusText(const QString& text);

	/// Emitted for a $PAIR001 acknowledgement: command id and result code.
	void commandAcknowledged(int command_id, int result);

private:
	/// Process a single complete NMEA sentence (including $ and checksum).
	void processSentence(const QByteArray& sentence);

	void handleGGA(const char* sentence);
	void handleRMC(const char* sentence);
	void handleGSA(const char* sentence);
	void handleGSV(const char* sentence);
	void handleGST(const char* sentence);
	/// Returns true when the sentence was recognized as a Quectel proprietary
	/// sentence, whether or not any observation could be extracted from it.
	bool handleProprietary(const QByteArray& sentence);
	void handleDrPva(const QByteArrayList& fields);
	void handleDrCal(const QByteArrayList& fields);

	QByteArray m_lineBuffer;
	Stats m_stats;
	bool m_gstValid = false;
	int m_gstHours = -1;
	int m_gstMinutes = -1;
	int m_gstSeconds = -1;
	float m_gstHAccuracy = NAN;
	float m_gstVAccuracy = NAN;

	static constexpr int kMaxLineLength = 256;  // NMEA max is 82, generous buffer
};


}  // namespace OpenOrienteering

#endif
