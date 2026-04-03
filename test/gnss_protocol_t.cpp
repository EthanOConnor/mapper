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

#include "gnss/gnss_position.h"
#include "gnss/gnss_state.h"
#include "gnss/protocol/ubx_parser.h"
#include "gnss/protocol/ubx_messages.h"
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
	auto pos = spy[0][0].value<GnssPosition>();

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
	QCOMPARE(spy.last()[0].value<GnssPosition>().fixType, GnssFixType::NoFix);

	// 2D fix
	frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT,
	    buildNavPvtPayload(0, 0, 0, 2, 0x01, 1000, 1000, 5));
	parser.addData(frame);
	QCOMPARE(spy.last()[0].value<GnssPosition>().fixType, GnssFixType::Fix2D);

	// 3D fix
	frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT,
	    buildNavPvtPayload(0, 0, 0, 3, 0x01, 1000, 1000, 5));
	parser.addData(frame);
	QCOMPARE(spy.last()[0].value<GnssPosition>().fixType, GnssFixType::Fix3D);

	// DGPS (diffSoln set)
	frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT,
	    buildNavPvtPayload(0, 0, 0, 3, 0x03, 1000, 1000, 5));
	parser.addData(frame);
	QCOMPARE(spy.last()[0].value<GnssPosition>().fixType, GnssFixType::DGPS);

	// RTK float (carrSoln=1 → bits 7:6 = 01 = 0x40)
	frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT,
	    buildNavPvtPayload(0, 0, 0, 3, 0x41, 1000, 1000, 5));
	parser.addData(frame);
	QCOMPARE(spy.last()[0].value<GnssPosition>().fixType, GnssFixType::RtkFloat);

	// RTK fixed (carrSoln=2 → bits 7:6 = 10 = 0x80)
	frame = buildUbxFrame(Ubx::kClassNAV, Ubx::kIdNAV_PVT,
	    buildNavPvtPayload(0, 0, 0, 3, 0x81, 1000, 1000, 5));
	parser.addData(frame);
	QCOMPARE(spy.last()[0].value<GnssPosition>().fixType, GnssFixType::RtkFixed);
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
	QVERIFY(std::abs(spy[0][0].toFloat() - 1.56f) < 0.01f);  // gDOP
	QVERIFY(std::abs(spy[0][1].toFloat() - 1.20f) < 0.01f);  // pDOP
	QVERIFY(std::abs(spy[0][4].toFloat() - 0.75f) < 0.01f);  // hDOP
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
	QVERIFY(std::abs(spy[0][0].toFloat() - 0.0004f) < 1e-6f);  // covNN
	QVERIFY(std::abs(spy[0][1].toFloat() - 0.0001f) < 1e-6f);  // covNE
	QVERIFY(std::abs(spy[0][2].toFloat() - 0.0009f) < 1e-6f);  // covEE
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
	QCOMPARE(spy[0][0].toInt(), 2);   // totalUsed
	QCOMPARE(spy[0][1].toInt(), 3);   // totalVisible
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
	QCOMPARE(spy[0][0].toBool(), true);   // fixOK
	QCOMPARE(spy[0][1].toBool(), true);   // diffSoln
	QCOMPARE(spy[0][2].toInt(), 2);       // carrSoln (fixed)
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
	QCOMPARE(spy[0][0].toString(), QStringLiteral("HPG 1.51"));
	QCOMPARE(spy[0][1].toString(), QStringLiteral("00190000"));
	auto exts = spy[0][2].toStringList();
	QCOMPARE(exts.size(), 1);
	QCOMPARE(exts[0], QStringLiteral("FWVER=HPG 1.51"));
}


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


// ======== NMEA Parser Tests ========


void GnssProtocolTest::nmeaGgaParsing()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::positionObservation);

	// Standard GGA sentence with RTK fixed quality (4)
	QByteArray sentence("$GNGGA,123519.00,4807.038,N,01131.000,E,4,08,0.9,545.4,M,47.0,M,1.0,0000*55\r\n");
	parser.addData(sentence);

	QCOMPARE(spy.count(), 1);
	auto pos = spy[0][0].value<GnssPosition>();
	QCOMPARE(pos.fixType, GnssFixType::RtkFixed);
	QVERIFY(pos.valid);
	// 4807.038 N = 48 + 7.038/60 = 48.1173
	QVERIFY(std::abs(pos.latitude - 48.1173) < 0.001);
	QCOMPARE(pos.satellitesUsed, std::uint8_t(8));
}


void GnssProtocolTest::nmeaRmcParsing()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::positionObservation);

	QByteArray sentence("$GNRMC,123519.00,A,4807.038,N,01131.000,E,022.4,084.4,230326,003.1,W*53\r\n");
	parser.addData(sentence);

	QCOMPARE(spy.count(), 1);
	auto pos = spy[0][0].value<GnssPosition>();
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
	QVERIFY(std::abs(spy[0][0].toFloat() - 1.2f) < 0.01f);  // pDOP
	QVERIFY(std::abs(spy[0][1].toFloat() - 0.8f) < 0.01f);  // hDOP
	QVERIFY(std::abs(spy[0][2].toFloat() - 0.9f) < 0.01f);  // vDOP
}


void GnssProtocolTest::nmeaGsvParsing()
{
	NmeaParser parser;
	QSignalSpy spy(&parser, &NmeaParser::satelliteObservation);

	QByteArray sentence("$GPGSV,3,1,12,01,40,083,46,02,17,308,44,12,07,344,39,14,22,228,45*7A\r\n");
	parser.addData(sentence);

	QCOMPARE(spy.count(), 1);
	QCOMPARE(spy[0][0].toInt(), 12);  // 12 satellites in view
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


QTEST_GUILESS_MAIN(GnssProtocolTest)
