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

#include "gnss_protocol_t.h"

#include <cmath>
#include <cstring>

#include <QByteArray>
#include <QSignalSpy>
#include <QtTest>

#include "gnss/gnss_observation.h"
#include "gnss/gnss_position.h"
#include "gnss/gnss_fusion_engine.h"
#include "gnss/gnss_session.h"
#include "gnss/gnss_state.h"
#include "gnss/correction/ntrip_client.h"
#include "gnss/correction/ntrip_profile.h"
#include "gnss/protocol/ubx_parser.h"
#include "gnss/protocol/ubx_config.h"
#include "gnss/protocol/ubx_messages.h"
#include "gnss/hyfix_receiver.h"
#include "gnss/protocol/hyfix_protocol.h"
#include "gnss/protocol/nmea_parser.h"
#include "gnss/protocol/protocol_detector.h"
#include "gnss/protocol/rtcm_framer.h"

namespace OpenOrienteering {}
using namespace OpenOrienteering;


GnssProtocolTest::GnssProtocolTest(QObject* parent)
    : QObject(parent)
{}


// ---- Helper: build a complete UBX frame from class, id, and payload ----

static QByteArray buildUbxFrame(std::uint8_t msgClass, std::uint8_t msgId,
                                const QByteArray& payload)
{
	QByteArray frame;
	auto len = static_cast<std::uint16_t>(payload.size());

	frame.append(static_cast<char>(Ubx::kSyncChar1));
	frame.append(static_cast<char>(Ubx::kSyncChar2));
	frame.append(static_cast<char>(msgClass));
	frame.append(static_cast<char>(msgId));
	frame.append(static_cast<char>(len & 0xFF));
	frame.append(static_cast<char>((len >> 8) & 0xFF));
	frame.append(payload);

	// Compute Fletcher-8 checksum over class, id, length, payload
	std::uint8_t ckA = 0, ckB = 0;
	for (int i = 2; i < frame.size(); ++i)
	{
		ckA += static_cast<std::uint8_t>(frame[i]);
		ckB += ckA;
	}
	frame.append(static_cast<char>(ckA));
	frame.append(static_cast<char>(ckB));

	return frame;
}


// ---- Helper: build a NAV-PVT payload ----

static QByteArray buildNavPvtPayload(double lat, double lon, double heightM,
                                     std::uint8_t fixType, std::uint8_t flags,
                                     std::uint32_t hAccMm, std::uint32_t vAccMm,
                                     std::uint8_t numSV)
{
	QByteArray payload(92, '\0');
	auto* pvt = reinterpret_cast<Ubx::NavPvt*>(payload.data());
	pvt->iTOW = 123456000;
	pvt->year = 2026;
	pvt->month = 4;
	pvt->day = 1;
	pvt->hour = 12;
	pvt->min = 30;
	pvt->sec = 15;
	pvt->valid = 0x03;  // validDate + validTime
	pvt->fixType = fixType;
	pvt->flags = flags;
	pvt->numSV = numSV;
	pvt->lat = static_cast<std::int32_t>(lat * 1e7);
	pvt->lon = static_cast<std::int32_t>(lon * 1e7);
	pvt->height = static_cast<std::int32_t>(heightM * 1e3);
	pvt->hMSL = static_cast<std::int32_t>((heightM - 30.0) * 1e3);
	pvt->hAcc = hAccMm;
	pvt->vAcc = vAccMm;
	pvt->pDOP = 120;  // 1.20 scaled
	pvt->gSpeed = 1500;  // 1.5 m/s
	return payload;
}


#if defined(MAPPER_GNSS_USE_GLEAN)
static void putU32Le(QByteArray& payload, int offset, std::uint32_t value)
{
	payload[offset + 0] = static_cast<char>(value & 0xFF);
	payload[offset + 1] = static_cast<char>((value >> 8) & 0xFF);
	payload[offset + 2] = static_cast<char>((value >> 16) & 0xFF);
	payload[offset + 3] = static_cast<char>((value >> 24) & 0xFF);
}


static void putI32Le(QByteArray& payload, int offset, std::int32_t value)
{
	putU32Le(payload, offset, static_cast<std::uint32_t>(value));
}


static QByteArray buildNavHpPosLlhPayload(double lat, double lon, double heightM,
                                          std::int8_t latHp, std::int8_t lonHp,
                                          std::int8_t heightHp,
                                          std::uint32_t hAcc01Mm,
                                          std::uint32_t vAcc01Mm)
{
	QByteArray payload(36, '\0');
	payload[0] = 0;  // version
	payload[3] = 0;  // flags: valid LLH
	putU32Le(payload, 4, 123456000);
	putI32Le(payload, 8, static_cast<std::int32_t>(lon * 1e7));
	putI32Le(payload, 12, static_cast<std::int32_t>(lat * 1e7));
	putI32Le(payload, 16, static_cast<std::int32_t>(heightM * 1e3));
	putI32Le(payload, 20, static_cast<std::int32_t>((heightM - 30.0) * 1e3));
	payload[24] = static_cast<char>(lonHp);
	payload[25] = static_cast<char>(latHp);
	payload[26] = static_cast<char>(heightHp);
	payload[27] = static_cast<char>(heightHp);
	putU32Le(payload, 28, hAcc01Mm);
	putU32Le(payload, 32, vAcc01Mm);
	return payload;
}
#endif


// ---- Helper: build an RTCM frame ----

static QByteArray buildRtcmFrame(int messageType, const QByteArray& data)
{
	int msgLen = data.size() + 2;  // +2 for the message type bits in the data
	QByteArray frame;

	// Preamble
	frame.append(static_cast<char>(0xD3));
	// Reserved (6 bits = 0) + length (10 bits)
	frame.append(static_cast<char>((msgLen >> 8) & 0x03));
	frame.append(static_cast<char>(msgLen & 0xFF));
	// Message type encoded in first 12 bits of data
	frame.append(static_cast<char>((messageType >> 4) & 0xFF));
	frame.append(static_cast<char>(((messageType & 0x0F) << 4)));
	frame.append(data);

	// CRC-24Q (need to compute over the frame so far)
	// Use a simple implementation here matching RtcmFramer's table
	// For test simplicity, compute CRC using the same algorithm
	static constexpr std::uint32_t crc24q_table[256] = {
		0x000000, 0x864CFB, 0x8AD50D, 0x0C99F6, 0x93E6E1, 0x15AA1A, 0x1933EC, 0x9F7F17,
		0xA18139, 0x27CDC2, 0x2B5434, 0xAD18CF, 0x3267D8, 0xB42B23, 0xB8B2D5, 0x3EFE2E,
		0xC54E89, 0x430272, 0x4F9B84, 0xC9D77F, 0x56A868, 0xD0E493, 0xDC7D65, 0x5A319E,
		0x64CFB0, 0xE2834B, 0xEE1ABD, 0x685646, 0xF72951, 0x7165AA, 0x7DFC5C, 0xFBB0A7,
		0x0CD1E9, 0x8A9D12, 0x8604E4, 0x00481F, 0x9F3708, 0x197BF3, 0x15E205, 0x93AEFE,
		0xAD50D0, 0x2B1C2B, 0x2785DD, 0xA1C926, 0x3EB631, 0xB8FACA, 0xB4633C, 0x322FC7,
		0xC99F60, 0x4FD39B, 0x434A6D, 0xC50696, 0x5A7981, 0xDC357A, 0xD0AC8C, 0x56E077,
		0x681E59, 0xEE52A2, 0xE2CB54, 0x6487AF, 0xFBF8B8, 0x7DB443, 0x712DB5, 0xF7614E,
		0x19A3D2, 0x9FEF29, 0x9376DF, 0x153A24, 0x8A4533, 0x0C09C8, 0x00903E, 0x86DCC5,
		0xB822EB, 0x3E6E10, 0x32F7E6, 0xB4BB1D, 0x2BC40A, 0xAD88F1, 0xA11107, 0x275DFC,
		0xDCED5B, 0x5AA1A0, 0x563856, 0xD074AD, 0x4F0BBA, 0xC94741, 0xC5DEB7, 0x43924C,
		0x7D6C62, 0xFB2099, 0xF7B96F, 0x71F594, 0xEE8A83, 0x68C678, 0x645F8E, 0xE21375,
		0x15723B, 0x933EC0, 0x9FA736, 0x19EBCD, 0x8694DA, 0x00D821, 0x0C41D7, 0x8A0D2C,
		0xB4F302, 0x32BFF9, 0x3E260F, 0xB86AF4, 0x2715E3, 0xA15918, 0xADC0EE, 0x2B8C15,
		0xD03CB2, 0x567049, 0x5AE9BF, 0xDCA544, 0x43DA53, 0xC596A8, 0xC90F5E, 0x4F43A5,
		0x71BD8B, 0xF7F170, 0xFB6886, 0x7D247D, 0xE25B6A, 0x641791, 0x688E67, 0xEEC29C,
		0x3347A4, 0xB50B5F, 0xB992A9, 0x3FDE52, 0xA0A145, 0x26EDBE, 0x2A7448, 0xAC38B3,
		0x92C69D, 0x148A66, 0x181390, 0x9E5F6B, 0x01207C, 0x876C87, 0x8BF571, 0x0DB98A,
		0xF6092D, 0x7045D6, 0x7CDC20, 0xFA90DB, 0x65EFCC, 0xE3A337, 0xEF3AC1, 0x69763A,
		0x578814, 0xD1C4EF, 0xDD5D19, 0x5B11E2, 0xC46EF5, 0x42220E, 0x4EBBF8, 0xC8F703,
		0x3F964D, 0xB9DAB6, 0xB54340, 0x330FBB, 0xAC70AC, 0x2A3C57, 0x26A5A1, 0xA0E95A,
		0x9E1774, 0x185B8F, 0x14C279, 0x928E82, 0x0DF195, 0x8BBD6E, 0x872498, 0x016863,
		0xFAD8C4, 0x7C943F, 0x700DC9, 0xF64132, 0x693E25, 0xEF72DE, 0xE3EB28, 0x65A7D3,
		0x5B59FD, 0xDD1506, 0xD18CF0, 0x57C00B, 0xC8BF1C, 0x4EF3E7, 0x426A11, 0xC426EA,
		0x2AE476, 0xACA88D, 0xA0317B, 0x267D80, 0xB90297, 0x3F4E6C, 0x33D79A, 0xB59B61,
		0x8B654F, 0x0D29B4, 0x01B042, 0x87FCB9, 0x1883AE, 0x9ECF55, 0x9256A3, 0x141A58,
		0xEFAAFF, 0x69E604, 0x657FF2, 0xE33309, 0x7C4C1E, 0xFA00E5, 0xF69913, 0x70D5E8,
		0x4E2BC6, 0xC8673D, 0xC4FECB, 0x42B230, 0xDDCD27, 0x5B81DC, 0x57182A, 0xD154D1,
		0x26359F, 0xA07964, 0xACE092, 0x2AAC69, 0xB5D37E, 0x339F85, 0x3F0673, 0xB94A88,
		0x87B4A6, 0x01F85D, 0x0D61AB, 0x8B2D50, 0x145247, 0x921EBC, 0x9E874A, 0x18CBB1,
		0xE37B16, 0x6537ED, 0x69AE1B, 0xEFE2E0, 0x709DF7, 0xF6D10C, 0xFA48FA, 0x7C0401,
		0x42FA2F, 0xC4B6D4, 0xC82F22, 0x4E63D9, 0xD11CCE, 0x575035, 0x5BC9C3, 0xDD8538,
	};
	std::uint32_t crc = 0;
	for (int i = 0; i < frame.size(); ++i)
	{
		auto b = static_cast<std::uint8_t>(frame[i]);
		crc = ((crc << 8) & 0xFFFFFF) ^ crc24q_table[b ^ (crc >> 16)];
	}
	frame.append(static_cast<char>((crc >> 16) & 0xFF));
	frame.append(static_cast<char>((crc >> 8) & 0xFF));
	frame.append(static_cast<char>(crc & 0xFF));

	return frame;
}


// ======== UBX Parser Tests ========


void GnssProtocolTest::ubxFletcher8Checksum()
{
	// Build a known frame and verify it parses (checksum is correct)
	auto payload = buildNavPvtPayload(47.5, 8.5, 500.0, 3, 0x01, 5000, 8000, 12);
	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT, payload);

	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::positionObservation);
	parser.addData(frame);
	QCOMPARE(spy.count(), 1);
	QCOMPARE(parser.stats().checksumErrors, std::uint64_t(0));
}


void GnssProtocolTest::ubxSyncByteDetection()
{
	auto payload = buildNavPvtPayload(47.5, 8.5, 500.0, 3, 0x01, 5000, 8000, 12);
	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT, payload);

	// Verify sync bytes are at the expected positions
	QCOMPARE(static_cast<std::uint8_t>(frame[0]), Ubx::kSyncChar1);
	QCOMPARE(static_cast<std::uint8_t>(frame[1]), Ubx::kSyncChar2);
}


void GnssProtocolTest::ubxNavPvtParsing()
{
	// RTK fixed: lat 47.123456, lon 8.654321, height 450m, hAcc 14mm, 18 SVs
	auto payload = buildNavPvtPayload(47.123456, 8.654321, 450.0,
	                                  3,       // fixType = 3D
	                                  0x81,    // flags: gnssFixOK(0x01) + carrSoln=2(0x80) = RTK fixed
	                                  14, 28,  // hAcc=14mm, vAcc=28mm
	                                  18);
	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT, payload);

	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::positionObservation);
	parser.addData(frame);

	QCOMPARE(spy.count(), 1);
	auto obs = spy[0][0].value<GnssPositionObservation>();
	const auto& pos = obs.position;

	QCOMPARE(pos.fixType, GnssFixType::RtkFixed);
	QVERIFY(pos.valid);
	QVERIFY(std::abs(pos.latitude - 47.123456) < 1e-6);
	QVERIFY(std::abs(pos.longitude - 8.654321) < 1e-6);
	QVERIFY(std::abs(pos.altitude - 450.0) < 0.01);
	QVERIFY(std::abs(pos.hAccuracy - 0.014f) < 0.001f);
	QVERIFY(std::abs(pos.vAccuracy - 0.028f) < 0.001f);
	QCOMPARE(pos.satellitesUsed, std::uint8_t(18));
	QCOMPARE(pos.accuracyBasis, GnssAccuracyBasis::Sigma68);

	// P95 should be hAccuracy * 1.6213
	float expectedP95 = 0.014f * GnssPosition::kP95FromSigma68;
	QVERIFY(std::abs(pos.hAccuracyP95 - expectedP95) < 0.001f);
}


void GnssProtocolTest::ubxNavPvtFixClassification()
{
	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::positionObservation);

	// Test each fix type classification

	// No fix (gnssFixOK not set)
	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT,
	    buildNavPvtPayload(0, 0, 0, 3, 0x00, 1000, 1000, 5));
	parser.addData(frame);
	QCOMPARE(spy.last()[0].value<GnssPositionObservation>().position.fixType, GnssFixType::NoFix);

	// 2D fix
	frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT,
	    buildNavPvtPayload(0, 0, 0, 2, 0x01, 1000, 1000, 5));
	parser.addData(frame);
	QCOMPARE(spy.last()[0].value<GnssPositionObservation>().position.fixType, GnssFixType::Fix2D);

	// 3D fix
	frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT,
	    buildNavPvtPayload(0, 0, 0, 3, 0x01, 1000, 1000, 5));
	parser.addData(frame);
	QCOMPARE(spy.last()[0].value<GnssPositionObservation>().position.fixType, GnssFixType::Fix3D);

	// DGPS (diffSoln set)
	frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT,
	    buildNavPvtPayload(0, 0, 0, 3, 0x03, 1000, 1000, 5));
	parser.addData(frame);
	QCOMPARE(spy.last()[0].value<GnssPositionObservation>().position.fixType, GnssFixType::DGPS);

	// RTK float (carrSoln=1 → bits 7:6 = 01 = 0x40)
	frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT,
	    buildNavPvtPayload(0, 0, 0, 3, 0x41, 1000, 1000, 5));
	parser.addData(frame);
	QCOMPARE(spy.last()[0].value<GnssPositionObservation>().position.fixType, GnssFixType::RtkFloat);

	// RTK fixed (carrSoln=2 → bits 7:6 = 10 = 0x80)
	frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT,
	    buildNavPvtPayload(0, 0, 0, 3, 0x81, 1000, 1000, 5));
	parser.addData(frame);
	QCOMPARE(spy.last()[0].value<GnssPositionObservation>().position.fixType, GnssFixType::RtkFixed);
}


void GnssProtocolTest::ubxNavDopParsing()
{
	QByteArray payload(18, '\0');
	auto* dop = reinterpret_cast<Ubx::NavDop*>(payload.data());
	dop->gDOP = 156;   // 1.56
	dop->pDOP = 120;   // 1.20
	dop->tDOP = 80;    // 0.80
	dop->vDOP = 95;    // 0.95
	dop->hDOP = 75;    // 0.75
	dop->nDOP = 50;    // 0.50
	dop->eDOP = 55;    // 0.55

	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_DOP, payload);

	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::dopObservation);
	parser.addData(frame);

	QCOMPARE(spy.count(), 1);
	auto dopObs = spy[0][0].value<GnssDopObservation>();
	QVERIFY(std::abs(dopObs.gDOP - 1.56f) < 0.01f);
	QVERIFY(std::abs(dopObs.pDOP - 1.20f) < 0.01f);
	QVERIFY(std::abs(dopObs.hDOP - 0.75f) < 0.01f);
}


void GnssProtocolTest::ubxNavCovParsing()
{
	QByteArray payload(64, '\0');
	auto* cov = reinterpret_cast<Ubx::NavCov*>(payload.data());
	cov->version = 0;
	cov->posCovValid = 1;
	cov->posCovNN = 0.0004f;   // 0.02m std dev north
	cov->posCovNE = 0.0001f;
	cov->posCovEE = 0.0009f;   // 0.03m std dev east

	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_COV, payload);

	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::covarianceObservation);
	parser.addData(frame);

	QCOMPARE(spy.count(), 1);
	auto covObs = spy[0][0].value<GnssCovarianceObservation>();
	QVERIFY(std::abs(covObs.covNN - 0.0004f) < 1e-6f);
	QVERIFY(std::abs(covObs.covNE - 0.0001f) < 1e-6f);
	QVERIFY(std::abs(covObs.covEE - 0.0009f) < 1e-6f);
}


void GnssProtocolTest::ubxNavSatParsing()
{
	// Build NAV-SAT with 3 satellites, 2 used
	Ubx::NavSatHeader header = {};
	header.version = 1;
	header.numSvs = 3;

	Ubx::NavSatEntry entries[3] = {};
	entries[0].gnssId = 0; entries[0].svId = 1; entries[0].cno = 42; entries[0].flags = 0x08;  // svUsed
	entries[1].gnssId = 0; entries[1].svId = 3; entries[1].cno = 35; entries[1].flags = 0x08;  // svUsed
	entries[2].gnssId = 2; entries[2].svId = 5; entries[2].cno = 20; entries[2].flags = 0x00;  // not used

	QByteArray payload;
	payload.append(reinterpret_cast<const char*>(&header), sizeof(header));
	for (auto& entry : entries)
		payload.append(reinterpret_cast<const char*>(&entry), sizeof(entry));

	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_SAT, payload);

	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::satelliteObservation);
	parser.addData(frame);

	QCOMPARE(spy.count(), 1);
	auto satObs = spy[0][0].value<GnssSatelliteObservation>();
	QCOMPARE(satObs.satellitesUsed, 2);
	QCOMPARE(satObs.satellitesVisible, 3);
}


void GnssProtocolTest::ubxNavStatusParsing()
{
	QByteArray payload(16, '\0');
	auto* status = reinterpret_cast<Ubx::NavStatus*>(payload.data());
	status->gpsFix = 3;
	status->flags = 0x03;   // gpsFixOK + diffSoln
	status->flags2 = 0x80;  // carrSoln=2 (RTK fixed)
	status->ttff = 15000;

	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_STATUS, payload);

	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::statusObservation);
	parser.addData(frame);

	QCOMPARE(spy.count(), 1);
	auto statusObs = spy[0][0].value<GnssStatusObservation>();
	QCOMPARE(statusObs.fixOK, true);
	QCOMPARE(statusObs.diffSoln, true);
	QCOMPARE(statusObs.carrSoln, 2);
}


void GnssProtocolTest::ubxMonVerParsing()
{
	QByteArray payload(40 + 30, '\0');  // header + 1 extension
	auto* ver = reinterpret_cast<Ubx::MonVerHeader*>(payload.data());
	std::strncpy(ver->swVersion, "HPG 1.51", 30);
	std::strncpy(ver->hwVersion, "00190000", 10);

	char* ext = payload.data() + 40;
	std::strncpy(ext, "FWVER=HPG 1.51", 30);

	auto frame = buildUbxFrame(Ubx::kClassMON, Ubx::kIdMON_VER, payload);

	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::versionObservation);
	parser.addData(frame);

	QCOMPARE(spy.count(), 1);
	auto verObs = spy[0][0].value<GnssVersionObservation>();
	QCOMPARE(verObs.swVersion, QStringLiteral("HPG 1.51"));
	QCOMPARE(verObs.hwVersion, QStringLiteral("00190000"));
	QCOMPARE(verObs.extensions.size(), 1);
	QCOMPARE(verObs.extensions[0], QStringLiteral("FWVER=HPG 1.51"));
}


#if defined(MAPPER_GNSS_USE_GLEAN)
void GnssProtocolTest::ubxNavHpposllhMergesWithPvt()
{
	auto pvt = buildNavPvtPayload(47.1234567, 8.6543210, 450.0,
	                              3, 0x81, 14, 28, 18);
	auto hp = buildNavHpPosLlhPayload(47.1234567, 8.6543210, 450.0,
	                                  6, -4, 7, 12, 18);

	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::positionObservation);
	parser.addData(buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT, pvt));
	parser.addData(buildUbxFrame(Ubx::kClassNAV, 0x14, hp));

	QCOMPARE(spy.count(), 2);
	auto obs = spy.last()[0].value<GnssPositionObservation>();
	const auto& pos = obs.position;
	QCOMPARE(pos.fixType, GnssFixType::RtkFixed);
	QVERIFY(pos.valid);
	QVERIFY(std::abs(pos.latitude - (47.1234567 + 6e-9)) < 1e-10);
	QVERIFY(std::abs(pos.longitude - (8.6543210 - 4e-9)) < 1e-10);
	QVERIFY(std::abs(pos.altitude - 450.0007) < 1e-5);
	QVERIFY(std::abs(pos.hAccuracy - 0.0012f) < 1e-6f);
	QCOMPARE(obs.meta.horizontalResolutionM, 0.0002f);
}
#endif


void GnssProtocolTest::ubxPartialFrame()
{
	auto payload = buildNavPvtPayload(47.5, 8.5, 500.0, 3, 0x01, 5000, 8000, 12);
	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT, payload);

	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::positionObservation);

	// Feed first half
	parser.addData(frame.left(50));
	QCOMPARE(spy.count(), 0);

	// Feed second half
	parser.addData(frame.mid(50));
	QCOMPARE(spy.count(), 1);
}


void GnssProtocolTest::ubxBadChecksum()
{
	auto payload = buildNavPvtPayload(47.5, 8.5, 500.0, 3, 0x01, 5000, 8000, 12);
	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT, payload);

	// Corrupt the last byte (checksum B)
	frame[frame.size() - 1] = static_cast<char>(frame[frame.size() - 1] ^ 0xFF);

	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::positionObservation);
	parser.addData(frame);

	QCOMPARE(spy.count(), 0);
	QVERIFY(parser.stats().checksumErrors > 0);
}


void GnssProtocolTest::ubxResyncAfterGarbage()
{
	// Prepend garbage bytes before a valid frame
	QByteArray garbage(50, '\xAA');
	auto payload = buildNavPvtPayload(47.5, 8.5, 500.0, 3, 0x01, 5000, 8000, 12);
	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT, payload);

	QByteArray data = garbage + frame;

	UbxParser parser;
	QSignalSpy spy(&parser, &UbxParser::positionObservation);
	parser.addData(data);

	QCOMPARE(spy.count(), 1);
	QVERIFY(parser.stats().syncResyncCount > 0);
}


void GnssProtocolTest::ubxInitConfiguresBothReceiverUarts()
{
	auto sequence = UbxConfig::buildInitSequence();
	QVERIFY(sequence.size() >= 2);
	const auto& frame = sequence.constFirst();
	QCOMPARE(static_cast<std::uint8_t>(frame[2]), UbxConfig::kClassCfg);
	QCOMPARE(static_cast<std::uint8_t>(frame[3]), UbxConfig::kIdCfgValset);

	QSet<std::uint32_t> keys;
	// Every item in this initialization request is a U1 configuration item.
	for (int offset = 10; offset + 4 < frame.size() - 2; offset += 5)
	{
		auto key = static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame[offset]))
		         | static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame[offset + 1])) << 8
		         | static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame[offset + 2])) << 16
		         | static_cast<std::uint32_t>(static_cast<std::uint8_t>(frame[offset + 3])) << 24;
		keys.insert(key);
	}

	for (auto key : {
	       0x10740001u, 0x10760001u,  // UBX output protocol, UART1/2
	       0x20910007u, 0x20910008u,  // NAV-PVT, UART1/2
	       0x20910034u, 0x20910035u,  // NAV-HPPOSLLH, UART1/2
	       0x20910084u, 0x20910085u,  // NAV-COV, UART1/2
	       0x20910039u, 0x2091003au,  // NAV-DOP, UART1/2
	       0x2091001bu, 0x2091001cu,  // NAV-STATUS, UART1/2
	       0x20910016u, 0x20910017u,  // NAV-SAT, UART1/2
	       0x10730004u, 0x10750004u,  // RTCM input, UART1/2
	       0x10730001u, 0x10750001u,  // UBX input, UART1/2
	     })
		QVERIFY2(keys.contains(key), qPrintable(QStringLiteral("Missing key 0x%1").arg(key, 8, 16, QLatin1Char('0'))));
}


void GnssProtocolTest::sessionReportsRawReceiverTrafficBeforePositionParsing()
{
	GnssSession session;
	QSignalSpy stateSpy(&session, &GnssSession::stateChanged);
	QByteArray undecodable(16, '\x55');
	session.feedData(undecodable);

	QCOMPARE(session.currentState().transportState, GnssTransportState::Connected);
	QCOMPARE(session.currentState().receiverBytesReceived, qint64(16));
	QVERIFY(session.currentState().lastReceiverDataTime.isValid());
	QCOMPARE(session.currentState().protocol, GnssProtocol::Unknown);
	QVERIFY(!session.currentState().solution.hasFreshPosition);
	QVERIFY(stateSpy.count() >= 2);
}


void GnssProtocolTest::sessionRedetectsProtocolAfterStartupNoise()
{
	GnssSession session;
	session.feedData(QByteArray(16, '\x55'));
	QCOMPARE(session.currentState().protocol, GnssProtocol::Unknown);

	auto payload = buildNavPvtPayload(47.5, 8.5, 500.0,
	                                  3, 0x01, 30, 50, 17);
	session.feedData(buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT, payload));

	QCOMPARE(session.currentState().protocol, GnssProtocol::UBX);
	QVERIFY(session.currentState().solution.hasFreshPosition);
	QCOMPARE(session.currentState().solution.position.satellitesUsed,
	         std::uint8_t(17));
}


// ======== NMEA Parser Tests ========


void GnssProtocolTest::nmeaGgaParsing()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::positionObservation);

	// Standard GGA sentence with RTK fixed quality (4)
	QByteArray sentence("$GNGGA,123519.00,4807.038,N,01131.000,E,4,08,0.9,545.4,M,47.0,M,1.0,0000*55\r\n");
	parser.addData(sentence);

	QCOMPARE(spy.count(), 1);
	auto obs = spy[0][0].value<GnssPositionObservation>();
	const auto& pos = obs.position;
	QCOMPARE(pos.fixType, GnssFixType::RtkFixed);
	QVERIFY(pos.valid);
	// 4807.038 N = 48 + 7.038/60 = 48.1173
	QVERIFY(std::abs(pos.latitude - 48.1173) < 0.001);
	QCOMPARE(pos.satellitesUsed, std::uint8_t(8));
}


void GnssProtocolTest::nmeaCoordinatePrecision()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::positionObservation);

	// Five decimal places in NMEA minutes resolve to about 2 cm here. A float
	// coordinate conversion would quantize this position by roughly 0.5 m.
	parser.addData(QByteArray(
	  "$GNGGA,123519.00,4735.44912,N,12213.59378,W,4,12,0.6,17.1,M,-19.1,M,1.0,0000*5D\r\n"));

	QCOMPARE(spy.count(), 1);
	auto observation = spy[0][0].value<GnssPositionObservation>();
	const double expected_latitude = 47.0 + 35.44912 / 60.0;
	const double expected_longitude = -(122.0 + 13.59378 / 60.0);
	QVERIFY(std::abs(observation.position.latitude - expected_latitude) < 1e-9);
	QVERIFY(std::abs(observation.position.longitude - expected_longitude) < 1e-9);
	QVERIFY(std::abs(double(float(expected_latitude)) - expected_latitude) > 1e-7);
}


void GnssProtocolTest::nmeaGstAccuracy()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::positionObservation);

	parser.addData(QByteArray(
	  "$GNGST,123519.00,0.8,0.02,0.01,45.0,0.012,0.014,0.030*7B\r\n"));
	parser.addData(QByteArray(
	  "$GNGGA,123519.00,4735.44912,N,12213.59378,W,4,12,0.6,17.1,M,-19.1,M,1.0,0000*5D\r\n"));

	QCOMPARE(spy.count(), 1);
	auto observation = spy[0][0].value<GnssPositionObservation>();
	QVERIFY(std::abs(observation.position.hAccuracy
	                 - std::hypot(0.012f, 0.014f)) < 1e-5f);
	QVERIFY(std::abs(observation.position.vAccuracy - 0.030f) < 1e-5f);
	QVERIFY(!observation.meta.accuracyDerived);

	// Never carry receiver statistics into a later fix epoch.
	parser.addData(QByteArray(
	  "$GNGGA,123520.00,4735.44912,N,12213.59378,W,4,12,0.6,17.1,M,-19.1,M,1.0,0000*57\r\n"));
	QCOMPARE(spy.count(), 2);
	QVERIFY(spy[1][0].value<GnssPositionObservation>().meta.accuracyDerived);
}


void GnssProtocolTest::nmeaRtkUereFallback()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::positionObservation);
	parser.addData(QByteArray(
	  "$GNGGA,123519.00,4735.44912,N,12213.59378,W,4,12,0.6,17.1,M,-19.1,M,1.0,0000*5D\r\n"));

	QCOMPARE(spy.count(), 1);
	auto observation = spy[0][0].value<GnssPositionObservation>();
	QCOMPARE(observation.position.fixType, GnssFixType::RtkFixed);
	QVERIFY(std::abs(observation.position.hAccuracy - 0.6f * 0.05f) < 1e-5f);
}


void GnssProtocolTest::nmeaRmcParsing()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::positionObservation);

	QByteArray sentence("$GNRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230326,003.1,W*53\r\n");
	parser.addData(sentence);

	QCOMPARE(spy.count(), 1);
	auto obs = spy[0][0].value<GnssPositionObservation>();
	const auto& pos = obs.position;
	QVERIFY(pos.valid);
	// Speed: 22.4 knots = 11.52 m/s
	QVERIFY(std::abs(pos.groundSpeed - 11.52f) < 0.1f);
}


void GnssProtocolTest::nmeaGsaParsing()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::dopObservation);

	QByteArray sentence("$GNGSA,A,3,01,02,03,04,05,06,07,08,,,,,1.2,0.8,0.9*26\r\n");
	parser.addData(sentence);

	QCOMPARE(spy.count(), 1);
	auto dopObs = spy[0][0].value<GnssDopObservation>();
	QVERIFY(std::abs(dopObs.pDOP - 1.2f) < 0.01f);
	QVERIFY(std::abs(dopObs.hDOP - 0.8f) < 0.01f);
	QVERIFY(std::abs(dopObs.vDOP - 0.9f) < 0.01f);
}


void GnssProtocolTest::nmeaGsvParsing()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::satelliteObservation);

	QByteArray sentence("$GPGSV,3,1,12,01,40,083,46,02,17,308,44,12,07,344,39,14,22,228,45*7A\r\n");
	parser.addData(sentence);

	QCOMPARE(spy.count(), 1);
	auto satObs = spy[0][0].value<GnssSatelliteObservation>();
	QCOMPARE(satObs.satellitesVisible, 12);
}


void GnssProtocolTest::nmeaBadChecksum()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::positionObservation);

	// Corrupt the checksum
	QByteArray sentence("$GNGGA,123519.00,4807.038,N,01131.000,E,4,08,0.9,545.4,M,47.0,M,1.0,0000*FF\r\n");  // FF is wrong checksum (correct is 55)
	parser.addData(sentence);

	QCOMPARE(spy.count(), 0);
	QVERIFY(parser.stats().checksumErrors > 0);
}


// ======== Protocol Detector Tests ========


void GnssProtocolTest::detectUbx()
{
	auto payload = buildNavPvtPayload(47.5, 8.5, 500.0, 3, 0x01, 5000, 8000, 12);
	auto frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT, payload);

	QCOMPARE(ProtocolDetector::detect(frame), GnssProtocol::UBX);
}


void GnssProtocolTest::detectNmea()
{
	QByteArray data("$GNGGA,123519.00,4807.038,N,01131.000,E,1,08,0.9,545.4,M,47.0,M,,*7F\r\n");
	QCOMPARE(ProtocolDetector::detect(data), GnssProtocol::NMEA);
}


void GnssProtocolTest::detectMixed()
{
	QByteArray nmea("$GNGGA,123519.00,4807.038,N,01131.000,E,1,08,0.9,545.4,M,47.0,M,,*7F\r\n");
	auto payload = buildNavPvtPayload(47.5, 8.5, 500.0, 3, 0x01, 5000, 8000, 12);
	auto ubxFrame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT, payload);

	QByteArray mixed = nmea + ubxFrame;
	QCOMPARE(ProtocolDetector::detect(mixed), GnssProtocol::Mixed);
}


void GnssProtocolTest::detectUnknown()
{
	QByteArray garbage(64, '\xAA');
	QCOMPARE(ProtocolDetector::detect(garbage), GnssProtocol::Unknown);

	// Too short
	QByteArray tiny("$G");
	QCOMPARE(ProtocolDetector::detect(tiny), GnssProtocol::Unknown);
}


void GnssProtocolTest::detectRtcm3()
{
	auto frame = buildRtcmFrame(1077, QByteArray(10, '\0'));
#if defined(MAPPER_GNSS_USE_GLEAN_WIRE)
	QCOMPARE(ProtocolDetector::detect(frame), GnssProtocol::RTCM3);
#else
	QCOMPARE(ProtocolDetector::detect(frame), GnssProtocol::Unknown);
#endif
}


// ======== RTCM Framer Tests ========


void GnssProtocolTest::rtcmFrameValidation()
{
	QByteArray data(10, '\x00');
	auto frame = buildRtcmFrame(1077, data);

	RtcmFramer framer;
	QSignalSpy spy(&framer, &RtcmFramer::frameValidated);
	framer.addData(frame);

	QCOMPARE(spy.count(), 1);
}


void GnssProtocolTest::rtcmBadCrc()
{
	QByteArray data(10, '\x00');
	auto frame = buildRtcmFrame(1077, data);

	// Corrupt CRC
	frame[frame.size() - 1] = static_cast<char>(frame[frame.size() - 1] ^ 0xFF);

	RtcmFramer framer;
	QSignalSpy validSpy(&framer, &RtcmFramer::frameValidated);
	QSignalSpy errorSpy(&framer, &RtcmFramer::crcError);
	framer.addData(frame);

	QCOMPARE(validSpy.count(), 0);
	QCOMPARE(errorSpy.count(), 1);
}


void GnssProtocolTest::rtcmMessageTypeExtraction()
{
	QByteArray data(10, '\x00');
	auto frame = buildRtcmFrame(1077, data);

	RtcmFramer framer;
	QSignalSpy spy(&framer, &RtcmFramer::frameValidated);
	framer.addData(frame);

	QCOMPARE(spy.count(), 1);
	QCOMPARE(spy[0][0].toInt(), 1077);
}


// ======== NTRIP Profile Tests ========


void GnssProtocolTest::ntripEmptyBasicAuthForRtkdata()
{
	NtripProfile profile;
	profile.casterHost = QStringLiteral("rtkdata.online");
	profile.casterPort = 11634;
	profile.mountpoint = QStringLiteral("TFH_ITRF2020");

	QVERIFY(ntripProfileRequiresEmptyBasicAuthorization(profile));
	QVERIFY(ntripProfileShouldSendAuthorization(profile));
	QCOMPARE(ntripProfileBasicAuthorizationValue(profile), QByteArrayLiteral("Og=="));

	NtripProfile genericProfile;
	genericProfile.casterHost = QStringLiteral("example.com");
	genericProfile.mountpoint = QStringLiteral("NOAUTH");
	QVERIFY(!ntripProfileShouldSendAuthorization(genericProfile));

	genericProfile.sendEmptyBasicAuth = true;
	QVERIFY(ntripProfileShouldSendAuthorization(genericProfile));
	QCOMPARE(ntripProfileBasicAuthorizationValue(genericProfile), QByteArrayLiteral("Og=="));
}


void GnssProtocolTest::ntripBasicAuthCredentials()
{
	NtripProfile profile;
	profile.casterHost = QStringLiteral("example.com");
	profile.mountpoint = QStringLiteral("VRS");
	profile.username = QStringLiteral("user");
	profile.password = QStringLiteral("pass");

	QVERIFY(!ntripProfileRequiresEmptyBasicAuthorization(profile));
	QVERIFY(ntripProfileShouldSendAuthorization(profile));
	QCOMPARE(ntripProfileBasicAuthorizationValue(profile), QByteArrayLiteral("dXNlcjpwYXNz"));
}


void GnssProtocolTest::ntripProfileNormalization()
{
	NtripProfile urlProfile;
	urlProfile.casterHost = QStringLiteral(" http://rtkdata.online:11634/TFH_ITRF2020 ");

	auto normalizedUrl = ntripProfileNormalized(urlProfile);
	QCOMPARE(normalizedUrl.casterHost, QStringLiteral("rtkdata.online"));
	QCOMPARE(normalizedUrl.casterPort, quint16(11634));
	QCOMPARE(normalizedUrl.mountpoint, QStringLiteral("TFH_ITRF2020"));
	QVERIFY(!normalizedUrl.useTls);
	QVERIFY(ntripProfileRequiresEmptyBasicAuthorization(normalizedUrl));

	NtripProfile splitProfile;
	splitProfile.casterHost = QStringLiteral("rtkdata.online:11634");
	splitProfile.mountpoint = QStringLiteral("/TFH_ITRF2020");

	auto normalizedSplit = ntripProfileNormalized(splitProfile);
	QCOMPARE(normalizedSplit.casterHost, QStringLiteral("rtkdata.online"));
	QCOMPARE(normalizedSplit.casterPort, quint16(11634));
	QCOMPARE(normalizedSplit.mountpoint, QStringLiteral("TFH_ITRF2020"));

	NtripProfile pathProfile;
	pathProfile.casterHost = QStringLiteral("rtkdata.online:11634/TFH_ITRF2020");

	auto normalizedPath = ntripProfileNormalized(pathProfile);
	QCOMPARE(normalizedPath.casterHost, QStringLiteral("rtkdata.online"));
	QCOMPARE(normalizedPath.casterPort, quint16(11634));
	QCOMPARE(normalizedPath.mountpoint, QStringLiteral("TFH_ITRF2020"));
}


void GnssProtocolTest::ntripBodyPreambleFilterStripsIcy()
{
	QByteArray rtcm;
	rtcm.append(static_cast<char>(0xD3));
	rtcm.append(static_cast<char>(0x00));
	rtcm.append(static_cast<char>(0x01));
	rtcm.append(static_cast<char>(0x42));

	NtripBodyPreambleFilter filter;
	QCOMPARE(filter.filter(QByteArrayLiteral("ICY 200 ")), QByteArray{});
	QCOMPARE(filter.filter(QByteArrayLiteral("OK\r\n") + rtcm), rtcm);

	QByteArray nextFrame;
	nextFrame.append(static_cast<char>(0xD3));
	nextFrame.append(static_cast<char>(0x00));
	nextFrame.append(static_cast<char>(0x02));
	nextFrame.append(static_cast<char>(0x43));
	QCOMPARE(filter.filter(nextFrame), nextFrame);

	NtripBodyPreambleFilter plainFilter;
	QCOMPARE(plainFilter.filter(rtcm), rtcm);
}


// ======== P95 Computation Tests ========


void GnssProtocolTest::p95FromSigma68()
{
	float reported = 0.014f;  // 14mm from UBX hAcc
	float p95 = GnssPosition::toP95(reported, GnssAccuracyBasis::Sigma68);
	float expected = reported * GnssPosition::kP95FromSigma68;
	QVERIFY(std::abs(p95 - expected) < 1e-6f);
	// Verify the constant is approximately right: 0.014 * 1.62 ~ 0.0227
	QVERIFY(std::abs(p95 - 0.0227f) < 0.001f);
}


void GnssProtocolTest::p95FromCep50()
{
	float reported = 0.020f;
	float p95 = GnssPosition::toP95(reported, GnssAccuracyBasis::CEP50);
	float expected = reported * GnssPosition::kP95FromCEP50;
	QVERIFY(std::abs(p95 - expected) < 1e-6f);
	// 0.020 * 2.079 ~ 0.0416
	QVERIFY(std::abs(p95 - 0.0416f) < 0.001f);
}


void GnssProtocolTest::p95Ellipse()
{
	GnssPosition pos;

	// Symmetric case: equal variance in N and E, no cross-correlation
	// sigma_N = sigma_E = 0.02m → variance = 0.0004
	pos.computeP95Ellipse(0.0004f, 0.0f, 0.0004f);
	QVERIFY(pos.ellipseAvailable);

	// For circular case, semi-major ≈ semi-minor
	QVERIFY(std::abs(pos.ellipseSemiMajorP95 - pos.ellipseSemiMinorP95) < 0.001f);

	// Expected: sqrt(0.0004 * 5.9915) ≈ sqrt(0.002397) ≈ 0.04896m
	QVERIFY(std::abs(pos.ellipseSemiMajorP95 - 0.04896f) < 0.002f);

	// Asymmetric case: more error east than north
	GnssPosition pos2;
	pos2.computeP95Ellipse(0.0001f, 0.0f, 0.0009f);
	QVERIFY(pos2.ellipseAvailable);
	QVERIFY(pos2.ellipseSemiMajorP95 > pos2.ellipseSemiMinorP95);

	// NAN handling
	float nanVal = GnssPosition::toP95(NAN, GnssAccuracyBasis::Sigma68);
	QVERIFY(std::isnan(nanVal));
}


// ---- HYFIX GEO-PULSE ----
//
// The literal sentences and replies below are verbatim captures from a
// GEO-PULSE running GPv2 3.8.2 (GNSS firmware 11.04 / R11A04S_CSA2) over its
// USB serial endpoint on 2026-08-27.

void GnssProtocolTest::hyfixDeviceNameRecognition()
{
	QVERIFY(HyfixProtocol::isHyfixDeviceName(QStringLiteral("GEOPULSE_38182BF816ED")));
	QVERIFY(HyfixProtocol::isHyfixDeviceName(QStringLiteral("geopulse_38182bf816ed")));
	QVERIFY(HyfixProtocol::isHyfixDeviceName(QStringLiteral("LiteRTK_001122334455")));
	QVERIFY(!HyfixProtocol::isHyfixDeviceName(QStringLiteral("ArduSimple BLE")));
	QVERIFY(!HyfixProtocol::isHyfixDeviceName(QString{}));

	QCOMPARE(HyfixProtocol::friendlyName(QStringLiteral("GEOPULSE_38182BF816ED")),
	         QStringLiteral("HYFIX GEO-PULSE (38182BF816ED)"));
	// Unrelated names pass through untouched.
	QCOMPARE(HyfixProtocol::friendlyName(QStringLiteral("ArduSimple BLE")),
	         QStringLiteral("ArduSimple BLE"));
}


void GnssProtocolTest::hyfixCommandFraming()
{
	QCOMPARE(HyfixProtocol::queryVersion(), QByteArray("+HYFIX,VERSION?#\r\n"));
	QCOMPARE(HyfixProtocol::queryMessageConfig(), QByteArray("+HYFIX,GNSSMSG?#\r\n"));
	QCOMPARE(HyfixProtocol::setRoverMode(HyfixCorrectionLink::Bluetooth),
	         QByteArray("+HYFIX,WORKMODE,ROVER,NTRIPCLI,BT#\r\n"));
	QCOMPARE(HyfixProtocol::setRoverMode(HyfixCorrectionLink::UsbC),
	         QByteArray("+HYFIX,WORKMODE,ROVER,NTRIPCLI,USBC#\r\n"));
	QCOMPARE(HyfixProtocol::setNmeaIntervalMs(200),
	         QByteArray("+HYFIX,GNSSMSG,NMEA,200#\r\n"));

	// The pass-through wraps a Quectel sentence with its NMEA checksum. This
	// checksum was accepted by the receiver on the bench.
	QCOMPARE(HyfixProtocol::nmeaChecksum("PAIR050,333"), QByteArray("20"));
	QCOMPARE(HyfixProtocol::transparentGnssCommand("PAIR050,333"),
	         QByteArray("+HYFIX,TRANS,GNSS,$PAIR050,333*20#\r\n"));
	// Single-digit checksums keep their leading zero.
	QCOMPARE(HyfixProtocol::nmeaChecksum("A").size(), 2);
}


void GnssProtocolTest::hyfixReplyParsing()
{
	HyfixDeviceInfo info;
	HyfixReply reply;

	QVERIFY(!HyfixProtocol::isReplyLine("$GNGGA,193009.000,4728.045452,N"));
	QVERIFY(!HyfixProtocol::parseReply("$GNGGA,193009.000", reply));

	QVERIFY(HyfixProtocol::parseReply("+HYFIX,VERSION,GPv2-3.8.2@20260415,3.8.2,GPv2,v2.0#\r", reply));
	QCOMPARE(reply.verb, QStringLiteral("VERSION"));
	QVERIFY(HyfixProtocol::applyReply(reply, info));
	QVERIFY(info.identified);
	QCOMPARE(info.productBanner, QStringLiteral("GPv2-3.8.2@20260415"));
	QCOMPARE(info.productFirmware, QStringLiteral("3.8.2"));
	QCOMPARE(info.hardwareModel, QStringLiteral("GPv2"));

	QVERIFY(HyfixProtocol::parseReply("+HYFIX,GNSSVERSION,11.04,R11A04S_CSA2#", reply));
	QVERIFY(HyfixProtocol::applyReply(reply, info));
	QCOMPARE(info.gnssFirmware, QStringLiteral("11.04"));
	QCOMPARE(info.gnssFirmwareBuild, QStringLiteral("R11A04S_CSA2"));

	QVERIFY(HyfixProtocol::parseReply("+HYFIX,WORKMODE,ROVER,NTRIPCLI,USBC#", reply));
	QVERIFY(HyfixProtocol::applyReply(reply, info));
	QCOMPARE(info.workMode, QStringLiteral("ROVER"));
	QCOMPARE(info.correctionLink, QStringLiteral("USBC"));

	// The serial number is kept; the account-binding ciphertext beside it is not.
	QVERIFY(HyfixProtocol::parseReply("+HYFIX,SN,38182BF816ED,CR18io9aiHlzDUfmLGdHSg==#", reply));
	QVERIFY(HyfixProtocol::applyReply(reply, info));
	QCOMPARE(info.serialNumber, QStringLiteral("38182BF816ED"));
	QCOMPARE(reply.fields.size(), 2);

	// Re-applying the same reply reports no change.
	QVERIFY(!HyfixProtocol::applyReply(reply, info));

	QVERIFY(HyfixProtocol::parseReply("+HYFIX,GNSSANT,0#", reply));
	QVERIFY(HyfixProtocol::applyReply(reply, info));
	QCOMPARE(info.antennaGear, 0);
}


void GnssProtocolTest::hyfixMessageConfigReplies()
{
	// The four lines a GNSSMSG? query answers with.
	HyfixDeviceInfo info;
	HyfixReply reply;
	for (const char* line : {"+HYFIX,GNSSMSG,NMEA,200#",
	                         "+HYFIX,GNSSMSG,RTCM,0#",
	                         "+HYFIX,GNSSMSG,IMU,10#",
	                         "+HYFIX,GNSSMSG,DR,1000#"})
	{
		QVERIFY(HyfixProtocol::parseReply(QByteArray(line), reply));
		HyfixProtocol::applyReply(reply, info);
	}
	QCOMPARE(info.nmeaIntervalMs, 200);
	QCOMPARE(info.rtcmIntervalMs, 0);
	QCOMPARE(info.imuRateHz, 10);
	QCOMPARE(info.drIntervalMs, 1000);
	QVERIFY(info.lastError.isEmpty());

	// A rejected rate is reported, not silently ignored.
	QVERIFY(HyfixProtocol::parseReply("+HYFIX,GNSSMSG,ERR,-1(ESP_FAIL)#", reply));
	QVERIFY(HyfixProtocol::applyReply(reply, info));
	QVERIFY(info.lastError.contains(QStringLiteral("ESP_FAIL")));
	// The rejected value did not replace the configured one.
	QCOMPARE(info.nmeaIntervalMs, 200);
}


void GnssProtocolTest::hyfixRateSnapsToSupportedInterval()
{
	// The firmware accepts only these four intervals; ~3 Hz (333 ms) is not
	// among them and the module rejects it through the pass-through too.
	QCOMPARE(HyfixProtocol::supportedNmeaIntervalsMs(), (QVector<int>{100, 200, 500, 1000}));
	QCOMPARE(HyfixProtocol::nearestSupportedNmeaIntervalMs(333), 200);
	QCOMPARE(HyfixProtocol::nearestSupportedNmeaIntervalMs(1000), 1000);
	QCOMPARE(HyfixProtocol::nearestSupportedNmeaIntervalMs(50), 100);
	QCOMPARE(HyfixProtocol::nearestSupportedNmeaIntervalMs(10000), 1000);
	QCOMPARE(HyfixProtocol::kDefaultNmeaIntervalMs, 200);

	HyfixReceiver receiver;
	receiver.setNmeaIntervalMs(333);
	QCOMPARE(receiver.nmeaIntervalMs(), 200);
}


void GnssProtocolTest::hyfixIdentifiesFromStreamAndBringsUp()
{
	HyfixReceiver receiver;
	receiver.setCorrectionLink(HyfixCorrectionLink::UsbC);
	receiver.setNmeaIntervalMs(333);
	QSignalSpy writes(&receiver, &HyfixReceiver::writeRequested);

	receiver.begin();
	QVERIFY(!receiver.isIdentified());
	// The probe goes out after the settling delay and nothing else does.
	QVERIFY(writes.wait(3000));
	QCOMPARE(writes.count(), 1);
	QCOMPARE(writes.at(0).at(0).toByteArray(), HyfixProtocol::queryVersion());

	// NMEA in the stream must not be mistaken for a reply.
	receiver.handleIncomingData("$GNGGA,193009.000,4728.045452,N,12145.534330,W,1,06,1.28,121.529,M,-17.177,M,,*43\r\n");
	QVERIFY(!receiver.isIdentified());

	receiver.handleIncomingData("+HYFIX,VERSION,GPv2-3.8.2@20260415,3.8.2,GPv2,v2.0#\r\n");
	QVERIFY(receiver.isIdentified());
	QCOMPARE(receiver.info().productFirmware, QStringLiteral("3.8.2"));

	// The bring-up sequence follows, and it configures the link, the rate,
	// and the foot-survey dynamic model.
	QTRY_VERIFY_WITH_TIMEOUT(writes.count() >= 10, 15000);
	QByteArrayList sent;
	for (const auto& call : writes)
		sent.append(call.at(0).toByteArray());
	QVERIFY(sent.contains(HyfixProtocol::setRoverMode(HyfixCorrectionLink::UsbC)));
	QVERIFY(sent.contains(HyfixProtocol::setNmeaIntervalMs(200)));
	QVERIFY(sent.contains(HyfixProtocol::queryMessageConfig()));
	// $PAIR080,1 = Fitness, sent last so the rate change's GNSS engine
	// restart cannot swallow it. Checksum verified live.
	QVERIFY(sent.last() == QByteArray("+HYFIX,TRANS,GNSS,$PAIR080,1*2F#\r\n"));
	QCOMPARE(HyfixProtocol::setNavigationMode(HyfixProtocol::NavigationMode::Fitness),
	         QByteArray("+HYFIX,TRANS,GNSS,$PAIR080,1*2F#\r\n"));
	// The bring-up runs once, not once per reply.
	receiver.handleIncomingData("+HYFIX,GNSSANT,0#\r\n");
	const auto count_after = writes.count();
	QTest::qWait(200);
	QCOMPARE(writes.count(), count_after);
}


void GnssProtocolTest::hyfixPacesCorrectionsOverBluetooth()
{
	HyfixReceiver receiver;
	receiver.setCorrectionLink(HyfixCorrectionLink::Bluetooth);
	QVERIFY(receiver.pacesCorrections());

	// NTRIP starts with the transport, before the receiver has answered the
	// probe. A device whose advertised name already says GEO-PULSE must have
	// its corrections paced from the first block, not from the first reply.
	QVERIFY(!receiver.handlesCorrections());
	receiver.setExpected(true);
	QVERIFY(receiver.handlesCorrections());

	QSignalSpy writes(&receiver, &HyfixReceiver::writeRequested);

	// A caster block larger than one BLE write.
	const QByteArray block(1500, '\xd3');
	receiver.sendCorrections(block);

	// The first chunk leaves immediately, capped at the vendor chunk size.
	QCOMPARE(writes.count(), 1);
	QCOMPARE(writes.at(0).at(0).toByteArray().size(), HyfixProtocol::kCorrectionChunkBytes);
	QCOMPARE(receiver.pendingCorrectionBytes(), 1500 - HyfixProtocol::kCorrectionChunkBytes);

	// The rest is metered out rather than pushed at the receiver at once.
	QTRY_COMPARE_WITH_TIMEOUT(receiver.pendingCorrectionBytes(), 0, 2000);
	QCOMPARE(writes.count(), 3);
	qsizetype total = 0;
	for (const auto& call : writes)
		total += call.at(0).toByteArray().size();
	QCOMPARE(total, block.size());

	// A backlog beyond the cap is dropped from the front: stale corrections
	// are worse than none.
	receiver.sendCorrections(QByteArray(HyfixReceiver::kMaxQueuedCorrectionBytes + 4096, '\xd3'));
	QVERIFY(receiver.pendingCorrectionBytes() <= HyfixReceiver::kMaxQueuedCorrectionBytes);
	QVERIFY(receiver.droppedCorrectionBytes() > 0);
}


void GnssProtocolTest::hyfixWritesCorrectionsDirectlyOverSerial()
{
	// Only the BLE path needs metering; a serial link takes the block whole.
	HyfixReceiver receiver;
	receiver.setCorrectionLink(HyfixCorrectionLink::UsbC);
	QVERIFY(!receiver.pacesCorrections());
	QSignalSpy writes(&receiver, &HyfixReceiver::writeRequested);

	const QByteArray block(1500, '\xd3');
	receiver.sendCorrections(block);
	QCOMPARE(writes.count(), 1);
	QCOMPARE(writes.at(0).at(0).toByteArray(), block);
	QCOMPARE(receiver.pendingCorrectionBytes(), 0);

	QCOMPARE(HyfixReceiver::linkForTransport(QStringLiteral("Serial")), HyfixCorrectionLink::UsbC);
	QCOMPARE(HyfixReceiver::linkForTransport(QStringLiteral("BLE")), HyfixCorrectionLink::Bluetooth);
}


void GnssProtocolTest::nmeaDrPvaParsing()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::deadReckoningObservation);
	QSignalSpy sentences(&parser, &NmeaParser::sentenceDecoded);

	parser.addData("$PQTMDRPVA,1,168835,193009.000,1,47.46742420,-121.75890550,121.529,"
	               "-17.177,0.458,0.006,0.023,0.458,0.000000,0.000000,0.719060*64\r\n");
	QCOMPARE(spy.count(), 1);
	QCOMPARE(sentences.count(), 1);
	QCOMPARE(sentences.at(0).at(0).toString(), QStringLiteral("PQTMDRPVA"));

	const auto observation = spy.at(0).at(0).value<GnssDeadReckoningObservation>();
	QCOMPARE(observation.meta.source, GnssObservationSource::QuectelDrPva);
	QCOMPARE(observation.navigationType, 1);  // GNSS only
	QVERIFY(std::abs(observation.heading - 0.71906f) < 0.0001f);
	QCOMPARE(observation.roll, 0.0f);
	QCOMPARE(observation.pitch, 0.0f);

	// The no-fix form has every value field empty and must not be mistaken
	// for a zeroed attitude.
	spy.clear();
	parser.addData("$PQTMDRPVA,1,1000,163355.000,0,,,,,,,,,,,*7C\r\n");
	QCOMPARE(spy.count(), 1);
	const auto empty = spy.at(0).at(0).value<GnssDeadReckoningObservation>();
	QCOMPARE(empty.navigationType, 0);
	QVERIFY(std::isnan(empty.heading));
	QVERIFY(std::isnan(empty.roll));
}


void GnssProtocolTest::nmeaDrCalParsing()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::deadReckoningObservation);

	parser.addData("$PQTMDRCAL,1,0,1*5C\r\n");
	QCOMPARE(spy.count(), 1);
	const auto observation = spy.at(0).at(0).value<GnssDeadReckoningObservation>();
	QCOMPARE(observation.meta.source, GnssObservationSource::QuectelDrCal);
	QCOMPARE(observation.calibrationState, 0);  // not calibrated
	QCOMPARE(observation.navigationType, 1);    // GNSS only

	// A status text and a command acknowledgement are recognized too.
	QSignalSpy texts(&parser, &NmeaParser::receiverStatusText);
	QSignalSpy acks(&parser, &NmeaParser::commandAcknowledged);
	QSignalSpy sentences(&parser, &NmeaParser::sentenceDecoded);
	parser.addData("$PQTMTXT,1,01,01,01,0x002A0001,0x000A0001.*5C\r\n");
	parser.addData("$PAIR001,050,2*3C\r\n");
	QCOMPARE(texts.count(), 1);
	QCOMPARE(acks.count(), 1);
	QCOMPARE(acks.at(0).at(0).toInt(), 50);
	QCOMPARE(acks.at(0).at(1).toInt(), 2);

	// A GEO-PULSE restarting its GNSS engine truncates the stream
	// mid-sentence. minmea accepts a checksum-less sentence, so the fragment
	// must be rejected here or it would be counted as a distinct message type
	// and crowd the bounded statistics table.
	const auto counted = sentences.count();
	parser.addData("$PAIR0\r\n");
	QCOMPARE(sentences.count(), counted);
}


void GnssProtocolTest::nmeaDrSolutionTypeEncodingIsNormalized()
{
	// PQTMDRPVA's SolType and PQTMDRCAL's NavType swap the meanings of 2 and 3.
	// Both must arrive normalized to the NavType encoding
	// (2 = DR only, 3 = GNSS + DR), or a fused solution would be reported as
	// dead-reckoning-only and vice versa.
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::deadReckoningObservation);

	// SolType 2 = GNSS + DR.
	parser.addData("$PQTMDRPVA,1,75000,083737.000,2,31.12738291,117.26372910,34.212,5.267,"
	               "3.212,2.928,0.238,4.346,0.392663,1.300793,0.030088*5E\r\n");
	QCOMPARE(spy.count(), 1);
	QCOMPARE(spy.at(0).at(0).value<GnssDeadReckoningObservation>().navigationType, 3);

	// NavType 3 = GNSS + DR, already in the canonical encoding.
	spy.clear();
	parser.addData("$PQTMDRCAL,1,2,3*5C\r\n");
	QCOMPARE(spy.count(), 1);
	const auto cal = spy.at(0).at(0).value<GnssDeadReckoningObservation>();
	QCOMPARE(cal.navigationType, 3);
	QCOMPARE(cal.calibrationState, 2);
}


void GnssProtocolTest::nmeaDrDoesNotDemoteRtkFix()
{
	// PQTMDRPVA has no RTK fix type. Fusing it as a position would drop an
	// RTK-fixed GGA to a plain 3D fix at every epoch, so it must only
	// contribute dead-reckoning state.
	GnssFusionEngine fusion;
	NmeaParser parser;
	QObject::connect(&parser, &NmeaParser::positionObservation, &parser,
	                 [&fusion](const GnssPositionObservation& o) { fusion.ingest(o); });
	QObject::connect(&parser, &NmeaParser::deadReckoningObservation, &parser,
	                 [&fusion](const GnssDeadReckoningObservation& o) { fusion.ingest(o); });

	// GGA with quality 4 = RTK fixed.
	parser.addData("$GNGGA,193009.000,4728.045452,N,12145.534330,W,4,12,0.60,121.529,M,-17.177,M,1.0,0000*61\r\n");
	QCOMPARE(fusion.solution().position.fixType, GnssFixType::RtkFixed);

	parser.addData("$PQTMDRPVA,1,168835,193009.000,1,47.46742420,-121.75890550,121.529,"
	               "-17.177,0.458,0.006,0.023,0.458,0.000000,0.000000,0.719060*64\r\n");
	QCOMPARE(fusion.solution().position.fixType, GnssFixType::RtkFixed);
	QCOMPARE(fusion.solution().drNavigationType, 1);

	parser.addData("$PQTMDRCAL,1,0,1*5C\r\n");
	QCOMPARE(fusion.solution().position.fixType, GnssFixType::RtkFixed);
	QCOMPARE(fusion.solution().drCalibrationState, 0);
	// PQTMDRCAL carries no attitude and must not erase PQTMDRPVA's.
	QCOMPARE(fusion.solution().drNavigationType, 1);
	QVERIFY(!std::isnan(fusion.solution().attitudeHeading));

	// Nor may the next PQTMDRPVA erase PQTMDRCAL's calibration state: the two
	// sentences alternate every epoch, so a slot shared between them would
	// flip the calibration state to unknown at the DRPVA rate.
	parser.addData("$PQTMDRPVA,1,168835,193009.000,1,47.46742420,-121.75890550,121.529,"
	               "-17.177,0.458,0.006,0.023,0.458,0.000000,0.000000,0.719060*64\r\n");
	QCOMPARE(fusion.solution().drCalibrationState, 0);
	QCOMPARE(fusion.solution().drNavigationType, 1);
}


QTEST_GUILESS_MAIN(GnssProtocolTest)
