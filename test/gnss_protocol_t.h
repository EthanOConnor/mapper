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

#ifndef OPENORIENTEERING_GNSS_PROTOCOL_T_H
#define OPENORIENTEERING_GNSS_PROTOCOL_T_H

#include <QObject>


class GnssProtocolTest : public QObject
{
Q_OBJECT
public:
	explicit GnssProtocolTest(QObject* parent = nullptr);

private slots:
	// UBX parser tests
	void ubxFletcher8Checksum();
	void ubxSyncByteDetection();
	void ubxNavPvtParsing();
	void ubxNavPvtFixClassification();
	void ubxNavDopParsing();
	void ubxNavCovParsing();
	void ubxNavSatParsing();
	void ubxNavStatusParsing();
	void ubxMonVerParsing();
	void ubxPartialFrame();
	void ubxBadChecksum();
	void ubxResyncAfterGarbage();

	// NMEA parser tests
	void nmeaGgaParsing();
	void nmeaRmcParsing();
	void nmeaGsaParsing();
	void nmeaGsvParsing();
	void nmeaBadChecksum();

	// Protocol detector tests
	void detectUbx();
	void detectNmea();
	void detectMixed();
	void detectUnknown();

	// RTCM framer tests
	void rtcmFrameValidation();
	void rtcmBadCrc();
	void rtcmMessageTypeExtraction();

	// P95 computation tests
	void p95FromSigma68();
	void p95FromCep50();
	void p95Ellipse();
};

#endif
