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
#if defined(MAPPER_GNSS_USE_GLEAN)
	void ubxNavHpposllhMergesWithPvt();
#endif
	void ubxPartialFrame();
	void ubxBadChecksum();
	void ubxResyncAfterGarbage();
	void ubxInitConfiguresBothReceiverUarts();
	void sessionReportsRawReceiverTrafficBeforePositionParsing();
	void sessionRedetectsProtocolAfterStartupNoise();

	// NMEA parser tests
	void nmeaGgaParsing();
	void nmeaRmcParsing();
	void nmeaGsaParsing();
	void nmeaGsvParsing();
	void nmeaBadChecksum();
	void nmeaCoordinatePrecision();
	void nmeaGstAccuracy();
	void nmeaRtkUereFallback();

	// HYFIX GEO-PULSE tests
	void hyfixDeviceNameRecognition();
	void hyfixCommandFraming();
	void hyfixReplyParsing();
	void hyfixMessageConfigReplies();
	void hyfixRateSnapsToSupportedInterval();
	void hyfixIdentifiesFromStreamAndBringsUp();
	void hyfixPacesCorrectionsOverBluetooth();
	void hyfixWritesCorrectionsDirectlyOverSerial();
	void nmeaEpeAccuracy();
	void nmeaDrPvaParsing();
	void nmeaDrCalParsing();
	void nmeaDrSolutionTypeEncodingIsNormalized();
	void nmeaDrDoesNotDemoteRtkFix();

	// Protocol detector tests
	void detectUbx();
	void detectNmea();
	void detectMixed();
	void detectUnknown();
	void detectRtcm3();

	// RTCM framer tests
	void rtcmFrameValidation();
	void rtcmBadCrc();
	void rtcmMessageTypeExtraction();

	// NTRIP profile tests
	void ntripEmptyBasicAuthForRtkdata();
	void ntripBasicAuthCredentials();
	void ntripProfileNormalization();
	void ntripBodyPreambleFilterStripsIcy();

	// P95 computation tests
	void p95FromSigma68();
	void p95FromCep50();
	void p95Ellipse();
};

#endif
