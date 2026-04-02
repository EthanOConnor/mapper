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


#ifndef OPENORIENTEERING_UBX_MESSAGES_H
#define OPENORIENTEERING_UBX_MESSAGES_H

#include <cstdint>

namespace OpenOrienteering {

/// UBX protocol constants and message payload structures.
///
/// All structures use #pragma pack(1) to match the wire format exactly.
/// Field names, types, and offsets come from the u-blox F9 HPG 1.51 and
/// X20 HPG 2.02 interface descriptions.
///
/// Type conventions (matching u-blox docs):
///   U1 = uint8_t,  I1 = int8_t
///   U2 = uint16_t, I2 = int16_t
///   U4 = uint32_t, I4 = int32_t
///   X1/X2/X4 = bitfield (unsigned)
///   R4 = float (IEEE 754 single)
///   CH = char

namespace Ubx {


// ---- Frame structure ----
// Every UBX frame: [0xB5 0x62] [class] [id] [len_lo len_hi] [payload...] [ck_a ck_b]
// Checksum is Fletcher-8 over class, id, length, and payload bytes.

constexpr std::uint8_t kSyncChar1 = 0xB5;
constexpr std::uint8_t kSyncChar2 = 0x62;
constexpr int kFrameOverhead = 8;  // 2 sync + 1 class + 1 id + 2 length + 2 checksum


// ---- Message classes ----

constexpr std::uint8_t kClassNAV = 0x01;
constexpr std::uint8_t kClassMON = 0x0A;
constexpr std::uint8_t kClassCFG = 0x06;
constexpr std::uint8_t kClassACK = 0x05;


// ---- Message IDs ----

constexpr std::uint8_t kIdNAV_PVT    = 0x07;  // 0x01 0x07
constexpr std::uint8_t kIdNAV_DOP    = 0x04;  // 0x01 0x04
constexpr std::uint8_t kIdNAV_SAT    = 0x35;  // 0x01 0x35
constexpr std::uint8_t kIdNAV_COV    = 0x36;  // 0x01 0x36
constexpr std::uint8_t kIdNAV_STATUS = 0x03;  // 0x01 0x03
constexpr std::uint8_t kIdMON_VER    = 0x04;  // 0x0A 0x04


// ---- NAV-PVT (0x01 0x07) ----
// Navigation Position Velocity Time Solution
// Payload: 92 bytes (fixed). Identical for F9P and X20P.

#pragma pack(push, 1)

struct NavPvt
{
	std::uint32_t iTOW;      ///< GPS time of week (ms)
	std::uint16_t year;      ///< Year UTC
	std::uint8_t  month;     ///< Month 1..12
	std::uint8_t  day;       ///< Day 1..31
	std::uint8_t  hour;      ///< Hour 0..23
	std::uint8_t  min;       ///< Minute 0..59
	std::uint8_t  sec;       ///< Second 0..60 (60 = leap second)
	std::uint8_t  valid;     ///< Validity flags
	std::uint32_t tAcc;      ///< Time accuracy estimate (ns)
	std::int32_t  nano;      ///< Fraction of second (-1e9..1e9 ns)
	std::uint8_t  fixType;   ///< Fix type: 0=none, 1=DR, 2=2D, 3=3D, 4=GNSS+DR, 5=time
	std::uint8_t  flags;     ///< Fix status flags
	std::uint8_t  flags2;    ///< Additional flags
	std::uint8_t  numSV;     ///< Number of SVs used
	std::int32_t  lon;       ///< Longitude (deg * 1e-7)
	std::int32_t  lat;       ///< Latitude (deg * 1e-7)
	std::int32_t  height;    ///< Height above ellipsoid (mm)
	std::int32_t  hMSL;      ///< Height above mean sea level (mm)
	std::uint32_t hAcc;      ///< Horizontal accuracy estimate (mm)
	std::uint32_t vAcc;      ///< Vertical accuracy estimate (mm)
	std::int32_t  velN;      ///< North velocity (mm/s)
	std::int32_t  velE;      ///< East velocity (mm/s)
	std::int32_t  velD;      ///< Down velocity (mm/s)
	std::int32_t  gSpeed;    ///< Ground speed 2D (mm/s)
	std::int32_t  headMot;   ///< Heading of motion (deg * 1e-5)
	std::uint32_t sAcc;      ///< Speed accuracy estimate (mm/s)
	std::uint32_t headAcc;   ///< Heading accuracy estimate (deg * 1e-5)
	std::uint16_t pDOP;      ///< Position DOP (* 0.01)
	std::uint16_t flags3;    ///< Additional flags
	std::uint8_t  reserved0[4];
	std::int32_t  headVeh;   ///< Heading of vehicle (deg * 1e-5)
	std::int16_t  magDec;    ///< Magnetic declination (deg * 1e-2)
	std::uint16_t magAcc;    ///< Magnetic declination accuracy (deg * 1e-2)

	// -- Scaling helpers --

	double longitudeDeg() const { return lon * 1e-7; }
	double latitudeDeg()  const { return lat * 1e-7; }
	double heightM()      const { return height * 1e-3; }
	double heightMslM()   const { return hMSL * 1e-3; }
	float  hAccuracyM()   const { return hAcc * 1e-3f; }
	float  vAccuracyM()   const { return vAcc * 1e-3f; }
	float  groundSpeedMs() const { return gSpeed * 1e-3f; }
	float  headingMotionDeg() const { return headMot * 1e-5f; }
	float  speedAccuracyMs()  const { return sAcc * 1e-3f; }
	float  pDopScaled()   const { return pDOP * 0.01f; }

	// -- Flag extraction --

	/// True if the GNSS fix is valid (gnssFixOK).
	bool gnssFixOK() const { return (flags & 0x01) != 0; }
	/// True if differential corrections were applied.
	bool diffSoln()  const { return (flags & 0x02) != 0; }
	/// Carrier solution status: 0=no carrier, 1=float, 2=fixed.
	int  carrSoln()  const { return (flags >> 6) & 0x03; }

	/// True if date is valid.
	bool validDate() const { return (valid & 0x01) != 0; }
	/// True if time is valid.
	bool validTime() const { return (valid & 0x02) != 0; }
};

static_assert(sizeof(NavPvt) == 92, "NAV-PVT payload must be 92 bytes");


// ---- NAV-DOP (0x01 0x04) ----
// Dilution of Precision
// Payload: 18 bytes (fixed).

struct NavDop
{
	std::uint32_t iTOW;  ///< GPS time of week (ms)
	std::uint16_t gDOP;  ///< Geometric DOP (* 0.01)
	std::uint16_t pDOP;  ///< Position DOP (* 0.01)
	std::uint16_t tDOP;  ///< Time DOP (* 0.01)
	std::uint16_t vDOP;  ///< Vertical DOP (* 0.01)
	std::uint16_t hDOP;  ///< Horizontal DOP (* 0.01)
	std::uint16_t nDOP;  ///< Northing DOP (* 0.01)
	std::uint16_t eDOP;  ///< Easting DOP (* 0.01)

	float gDopScaled() const { return gDOP * 0.01f; }
	float pDopScaled() const { return pDOP * 0.01f; }
	float tDopScaled() const { return tDOP * 0.01f; }
	float vDopScaled() const { return vDOP * 0.01f; }
	float hDopScaled() const { return hDOP * 0.01f; }
	float nDopScaled() const { return nDOP * 0.01f; }
	float eDopScaled() const { return eDOP * 0.01f; }
};

static_assert(sizeof(NavDop) == 18, "NAV-DOP payload must be 18 bytes");


// ---- NAV-SAT (0x01 0x35) ----
// Satellite Information
// Payload: 8 + numSvs * 12 bytes (variable).

struct NavSatHeader
{
	std::uint32_t iTOW;       ///< GPS time of week (ms)
	std::uint8_t  version;    ///< Message version (0x01)
	std::uint8_t  numSvs;     ///< Number of satellites
	std::uint8_t  reserved0[2];
};

static_assert(sizeof(NavSatHeader) == 8, "NAV-SAT header must be 8 bytes");

struct NavSatEntry
{
	std::uint8_t  gnssId;  ///< GNSS identifier (matches GnssConstellation enum)
	std::uint8_t  svId;    ///< Satellite identifier
	std::uint8_t  cno;     ///< Carrier-to-noise ratio (dBHz)
	std::int8_t   elev;    ///< Elevation (-90..+90 deg)
	std::int16_t  azim;    ///< Azimuth (0..360 deg)
	std::int16_t  prRes;   ///< Pseudorange residual (* 0.1 m)
	std::uint32_t flags;   ///< Status flags

	/// Signal quality indicator (0-7).
	int qualityInd() const { return flags & 0x07; }
	/// True if this SV is used in the navigation solution.
	bool svUsed()    const { return (flags & 0x08) != 0; }
	/// Health: 0=unknown, 1=healthy, 2=unhealthy.
	int health()     const { return (flags >> 4) & 0x03; }
	/// True if differential corrections available.
	bool diffCorr()  const { return (flags & 0x40) != 0; }
	/// True if RTCM corrections used.
	bool rtcmCorrUsed() const { return (flags & 0x20000) != 0; }
	/// True if SPARTN corrections used.
	bool spartnCorrUsed() const { return (flags & 0x80000) != 0; }
};

static_assert(sizeof(NavSatEntry) == 12, "NAV-SAT entry must be 12 bytes");


// ---- NAV-COV (0x01 0x36) ----
// Covariance Matrices
// Payload: 64 bytes (fixed).

struct NavCov
{
	std::uint32_t iTOW;          ///< GPS time of week (ms)
	std::uint8_t  version;       ///< Message version (0x00)
	std::uint8_t  posCovValid;   ///< Position covariance valid
	std::uint8_t  velCovValid;   ///< Velocity covariance valid
	std::uint8_t  reserved0[9];
	float         posCovNN;      ///< Pos cov north-north (m^2)
	float         posCovNE;      ///< Pos cov north-east (m^2)
	float         posCovND;      ///< Pos cov north-down (m^2)
	float         posCovEE;      ///< Pos cov east-east (m^2)
	float         posCovED;      ///< Pos cov east-down (m^2)
	float         posCovDD;      ///< Pos cov down-down (m^2)
	float         velCovNN;      ///< Vel cov north-north (m^2/s^2)
	float         velCovNE;      ///< Vel cov north-east (m^2/s^2)
	float         velCovND;      ///< Vel cov north-down (m^2/s^2)
	float         velCovEE;      ///< Vel cov east-east (m^2/s^2)
	float         velCovED;      ///< Vel cov east-down (m^2/s^2)
	float         velCovDD;      ///< Vel cov down-down (m^2/s^2)
};

static_assert(sizeof(NavCov) == 64, "NAV-COV payload must be 64 bytes");


// ---- NAV-STATUS (0x01 0x03) ----
// Receiver Navigation Status
// Payload: 16 bytes (fixed).

struct NavStatus
{
	std::uint32_t iTOW;     ///< GPS time of week (ms)
	std::uint8_t  gpsFix;   ///< Fix type (same encoding as NavPvt::fixType)
	std::uint8_t  flags;    ///< Status flags
	std::uint8_t  fixStat;  ///< Fix status info
	std::uint8_t  flags2;   ///< Further info
	std::uint32_t ttff;     ///< Time to first fix (ms)
	std::uint32_t msss;     ///< Milliseconds since startup/reset

	bool gpsFixOK()  const { return (flags & 0x01) != 0; }
	bool diffSoln()  const { return (flags & 0x02) != 0; }
	/// Carrier solution: 0=none, 1=float, 2=fixed (from flags2 bits 7..6).
	int  carrSoln()  const { return (flags2 >> 6) & 0x03; }
	/// Spoofing detection: 0=unknown, 1=none, 2=indicated, 3=multiple.
	int  spoofDet()  const { return (flags2 >> 3) & 0x03; }
};

static_assert(sizeof(NavStatus) == 16, "NAV-STATUS payload must be 16 bytes");


// ---- MON-VER (0x0A 0x04) ----
// Receiver/Software Version
// Payload: 40 + N*30 bytes (variable).

struct MonVerHeader
{
	char swVersion[30];   ///< Software version (nul-terminated)
	char hwVersion[10];   ///< Hardware version (nul-terminated)
};

static_assert(sizeof(MonVerHeader) == 40, "MON-VER header must be 40 bytes");

/// Each extension string in MON-VER is 30 bytes, nul-terminated.
constexpr int kMonVerExtensionSize = 30;


#pragma pack(pop)

}  // namespace Ubx

}  // namespace OpenOrienteering

#endif
