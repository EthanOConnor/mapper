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


#ifndef OPENORIENTEERING_HYFIX_PROTOCOL_H
#define OPENORIENTEERING_HYFIX_PROTOCOL_H

#include <cstdint>

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace OpenOrienteering {


/// Which link the receiver should expect RTCM corrections to arrive on.
///
/// The GEO-PULSE reports this as the third element of its work mode, e.g.
/// `+HYFIX,WORKMODE,ROVER,NTRIPCLI,USBC#`. It is a property of the host link,
/// not of the correction source: in every one of these modes the receiver's
/// own NTRIP client stays stopped and we feed it RTCM ourselves.
enum class HyfixCorrectionLink : std::uint8_t
{
	Bluetooth = 0,  ///< `BT` — corrections written to the NUS RX characteristic
	UsbC      = 1,  ///< `USBC` — corrections written to the USB serial endpoint
	/// `WIFI` — placeholder only. No firmware acceptance of this token has
	/// been observed; the receiver's native network correction path reports
	/// `NTRIPCLI` in this field instead. Mapper never sends it.
	Wifi      = 2,
};


/// One decoded `+HYFIX,<verb>,<field>,...#` status or reply line.
struct HyfixReply
{
	QString verb;
	QStringList fields;
};


/// Receiver facts learned from `+HYFIX` replies and Quectel status sentences.
///
/// Every field is "unknown" until the receiver reports it: strings stay empty
/// and numbers stay negative.
struct HyfixDeviceInfo
{
	bool identified = false;       ///< A `+HYFIX` reply has been seen on this link

	// -- Identity (from VERSION / GNSSVERSION / SN) --
	QString serialNumber;          ///< e.g. "38182BF816ED"; matches the BLE name suffix
	QString productFirmware;       ///< e.g. "3.8.2"
	QString productBanner;         ///< e.g. "GPv2-3.8.2@20260415"
	QString hardwareModel;         ///< e.g. "GPv2"
	QString protocolVersion;       ///< e.g. "v2.0"
	QString gnssFirmware;          ///< Quectel firmware version, e.g. "11.04"
	QString gnssFirmwareBuild;     ///< e.g. "R11A04S_CSA2"

	// -- Mode and link state --
	QString workMode;              ///< e.g. "ROVER"
	QString correctionMode;        ///< e.g. "NTRIPCLI"
	QString correctionLink;        ///< e.g. "USBC", "BT"
	QString ntripClientState;      ///< Receiver-side NTRIP client: STOP/CONNECTING/...
	QString wifiState;

	// -- Output configuration (all intervals in milliseconds) --
	int nmeaIntervalMs = -1;
	int rtcmIntervalMs = -1;
	int drIntervalMs   = -1;
	int imuRateHz      = -1;
	int antennaGear    = -1;       ///< Re-radiation port gain step

	// -- Dead-reckoning state (from $PQTMDRCAL) --
	int drCalibrationState = -1;   ///< 0 uncalibrated, 1 light, 2 full, 3 full + heading
	int drNavType          = -1;   ///< 0 none, 1 GNSS only, 2 DR only, 3 GNSS + DR

	QString lastError;             ///< Text of the most recent `ERR` reply
};


/// The HYFIX GEO-PULSE (GP100, firmware product name `GPv2`) control protocol.
///
/// The receiver is an ESP32 MCU in front of a Quectel LC29H-class DR/RTK
/// module. The MCU owns the module's command surface and exposes its own
/// product-level protocol: UTF-8 lines of the form
///
///     +HYFIX,<verb>[,<arg>...]#<CR><LF>
///
/// on whichever link the host is attached to — the Nordic UART Service RX
/// characteristic over BLE, or the second CH342 USB serial endpoint over USB-C.
/// Replies use the same framing and arrive interleaved with the NMEA feed.
///
/// Everything here is verified against a GEO-PULSE running GPv2 3.8.2 (GNSS
/// firmware 11.04 / R11A04S_CSA2) over the USB serial endpoint; see
/// `cascadia/gnss/docs/on-hand-hardware.md` for the capture records.
class HyfixProtocol
{
public:
	/// BLE advertised names use these prefixes; GEO-PULSE units advertise
	/// `GEOPULSE_<12 hex>`, and the vendor app also accepts two sibling
	/// product names on the same protocol.
	static bool isHyfixDeviceName(const QString& name);

	/// A display name for an advertised or port name, e.g.
	/// "HYFIX GEO-PULSE (38182BF816ED)". Returns the input unchanged when it
	/// is not a recognized HYFIX name.
	static QString friendlyName(const QString& name);

	// ---- Command construction ----

	/// Wrap a command body: `+HYFIX,<body>#\r\n`.
	static QByteArray command(const QByteArray& body);

	static QByteArray queryVersion();
	static QByteArray queryGnssVersion();
	static QByteArray querySerialNumber();
	static QByteArray queryWorkMode();
	static QByteArray queryMessageConfig();
	static QByteArray queryNtripClientStatus();
	static QByteArray queryAntenna();

	/// `+HYFIX,WORKMODE,ROVER,NTRIPCLI,<link>#` — rover taking corrections
	/// from the host link. This leaves the receiver's own NTRIP client stopped.
	static QByteArray setRoverMode(HyfixCorrectionLink link);

	/// The WORKMODE token for a correction link: "BT", "USBC", "WIFI".
	static QString linkToken(HyfixCorrectionLink link);

	/// `+HYFIX,GNSSMSG,NMEA,<intervalMs>#` — position fix and NMEA output
	/// interval. Only the intervals in supportedNmeaIntervalsMs() are accepted;
	/// anything else is answered with `+HYFIX,GNSSMSG,ERR,-1(ESP_FAIL)#`.
	static QByteArray setNmeaIntervalMs(int interval_ms);

	/// `$PAIR080` navigation modes: the module's GNSS dynamic model.
	/// Verified live: setting Fitness answers `$PAIR001,080,0` and the
	/// readback (`$PAIR081`) reflects it; the position rate is unaffected.
	enum class NavigationMode : int
	{
		Normal     = 0,  ///< Driving-class dynamics (receiver default)
		Fitness    = 1,  ///< Walking/running; damps low-speed movement noise
		Stationary = 4,
		Drone      = 5,
		Swimming   = 7,
		Bike       = 9,
	};

	/// `$PAIR080,<mode>` through the pass-through: set the GNSS dynamic model.
	static QByteArray setNavigationMode(NavigationMode mode);

	/// `$PQTMCFGMSGRATE,W,PQTMEPE,1,2` through the pass-through: output the
	/// receiver's own estimated position error every fix. Verified live:
	/// $PQTMEPE v2 (north/east/down/2D/3D meters) follows at the position
	/// rate. Like all GNSSMSG-layer settings it resets on power cycle.
	static QByteArray enableEstimatedPositionError();

	/// `+HYFIX,TRANS,GNSS,<sentence>*<checksum>#` — pass a raw Quectel
	/// sentence through the MCU to the GNSS module. The MCU acknowledges with
	/// `+HYFIX,TRANS,GNSS,OK#` and the module answers separately, typically
	/// with `$PAIR001,<id>,<result>`.
	///
	/// `body` is the sentence without the leading `$` and without a checksum.
	static QByteArray transparentGnssCommand(const QByteArray& body);

	/// Intervals the GPv2 firmware accepts for the NMEA/position rate, in
	/// milliseconds, ascending. Measured on the bench: 1000 -> 1.01 Hz,
	/// 500 -> 2.04 Hz, 200 -> 4.92 Hz.
	static QVector<int> supportedNmeaIntervalsMs();

	/// The supported interval closest to `desired_ms`. Ties resolve to the
	/// shorter interval (the higher rate).
	static int nearestSupportedNmeaIntervalMs(int desired_ms);

	/// Default position rate for Mapper sessions: 200 ms (~5 Hz).
	///
	/// The nominal target is ~3 Hz, but the GPv2 firmware only accepts the
	/// discrete set above, and the module rejects arbitrary `$PAIR050`
	/// intervals even through the `TRANS` pass-through. 200 ms is the nearest
	/// supported interval at or above the target rate.
	static constexpr int kDefaultNmeaIntervalMs = 200;

	// ---- Correction pacing ----

	/// Maximum RTCM bytes per write to the receiver. The vendor app splits
	/// caster blocks at this size for a 517-byte negotiated BLE MTU.
	static constexpr int kCorrectionChunkBytes = 509;

	/// Delay the vendor app leaves between correction chunks.
	static constexpr int kCorrectionChunkIntervalMs = 80;

	// ---- Reply parsing ----

	/// Whether a line is a `+HYFIX` reply (leading whitespace is tolerated).
	static bool isReplyLine(const QByteArray& line);

	/// Decode a `+HYFIX,<verb>,...#` line. Returns false for anything else.
	static bool parseReply(const QByteArray& line, HyfixReply& reply);

	/// Fold a decoded reply into `info`. Returns true when `info` changed.
	static bool applyReply(const HyfixReply& reply, HyfixDeviceInfo& info);

	/// NMEA 0183 XOR checksum over `body` (no `$`, no `*`), as two hex digits.
	static QByteArray nmeaChecksum(const QByteArray& body);
};


}  // namespace OpenOrienteering

Q_DECLARE_METATYPE(OpenOrienteering::HyfixDeviceInfo)

#endif
