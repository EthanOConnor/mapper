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


#ifndef OPENORIENTEERING_HYFIX_RECEIVER_H
#define OPENORIENTEERING_HYFIX_RECEIVER_H

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include "protocol/hyfix_protocol.h"

namespace OpenOrienteering {


/// Drives a HYFIX GEO-PULSE across a connected transport.
///
/// The GEO-PULSE needs three things a generic NMEA receiver does not:
///
///  1. A product-level bring-up handshake. The receiver has to be told which
///     link its corrections will arrive on, and it only reports its firmware,
///     serial number, and output configuration when asked.
///  2. A position rate. It boots at 1 Hz, which is too slow for field survey.
///  3. Paced correction writes. Its BLE receive path drops bytes if RTCM is
///     pushed faster than roughly one 509-byte chunk per 80 ms, so corrections
///     are queued and metered rather than written straight through.
///
/// This class owns all three and stays transport-agnostic: it asks its owner to
/// write bytes and is fed the lines that come back.
class HyfixReceiver : public QObject
{
	Q_OBJECT

public:
	explicit HyfixReceiver(QObject* parent = nullptr);
	~HyfixReceiver() override;

	/// The link the receiver should expect corrections on. Derived from the
	/// transport type; see linkForTransport().
	void setCorrectionLink(HyfixCorrectionLink link);
	HyfixCorrectionLink correctionLink() const { return m_link; }

	/// Desired position/NMEA output interval in milliseconds. The value is
	/// snapped to the nearest interval the firmware accepts.
	void setNmeaIntervalMs(int interval_ms);
	int nmeaIntervalMs() const { return m_nmea_interval_ms; }

	/// GNSS dynamic model applied during bring-up. Defaults to Fitness:
	/// Mapper's survey use is on foot, where the low-speed model measurably
	/// helps and the driving-class default does not. Applied per session and
	/// never persisted to the receiver's NVM, so the vendor app finds the
	/// device unchanged.
	void setNavigationMode(HyfixProtocol::NavigationMode mode) { m_navigation_mode = mode; }
	HyfixProtocol::NavigationMode navigationMode() const { return m_navigation_mode; }

	/// Whether corrections need metering on this link. True for BLE only.
	bool pacesCorrections() const { return m_link == HyfixCorrectionLink::Bluetooth; }

	/// Probe the link for a GEO-PULSE. A single `+HYFIX,VERSION?#` goes out
	/// after a settling delay; the rest of the bring-up runs only once the
	/// receiver answers. The probe is inert for other receivers — it is
	/// neither a valid NMEA sentence nor a UBX frame — so it is safe to send
	/// on any link, which is what makes USB-serial GEO-PULSE units
	/// self-identifying. Safe to call again after a reconnect.
	void begin();

	/// Whether a GEO-PULSE has answered on this link.
	bool isIdentified() const { return m_info.identified; }

	/// Mark the device as a GEO-PULSE known from its advertised name, before
	/// any reply arrives. Correction pacing must not wait for the handshake:
	/// NTRIP starts with the transport, and the first caster blocks would
	/// otherwise be written unpaced — exactly the burst the pacer exists to
	/// prevent.
	void setExpected(bool expected) { m_expected = expected; }

	/// Whether corrections should be routed through this receiver's pacing.
	bool handlesCorrections() const { return m_expected || m_info.identified; }

	/// Cancel any pending handshake step and discard queued corrections.
	void stop();

	/// Feed raw receiver bytes. Only `+HYFIX` lines are consumed; everything
	/// else (NMEA, RTCM) is ignored here and handled by the normal parsers.
	void handleIncomingData(const QByteArray& data);

	/// Record dead-reckoning state decoded from $PQTMDRCAL.
	void setDeadReckoningState(int calibration_state, int navigation_type);

	/// Hand correction bytes to the receiver, metered when the link needs it.
	void sendCorrections(const QByteArray& rtcm);

	const HyfixDeviceInfo& info() const { return m_info; }

	/// Correction bytes discarded because the outbound queue was full.
	qint64 droppedCorrectionBytes() const { return m_dropped_bytes; }

	/// Bytes still waiting to be metered out to the receiver.
	int pendingCorrectionBytes() const { return int(m_correction_queue.size()); }

	/// The correction link implied by a GnssTransport::typeName().
	static HyfixCorrectionLink linkForTransport(const QString& transport_type);

	/// Outbound correction backlog cap. Corrections are only useful while
	/// fresh, so a backlog beyond a few seconds of caster output is dropped
	/// from the front rather than delivered late.
	static constexpr int kMaxQueuedCorrectionBytes = 32 * 1024;

signals:
	/// Bytes the owner should write to the transport.
	void writeRequested(const QByteArray& data);

	/// Emitted when any receiver fact changed.
	void infoChanged(const OpenOrienteering::HyfixDeviceInfo& info);

	/// Emitted for a receiver-reported error (e.g. a rejected command).
	void errorOccurred(const QString& message);

private:
	void enqueueCommand(int delay_ms, const QByteArray& command);
	void startBringUp();
	void sendNextCommand();
	void drainCorrections();
	void handleLine(const QByteArray& line);

	struct PendingCommand
	{
		int delay_ms;
		QByteArray command;
	};

	HyfixDeviceInfo m_info;
	HyfixCorrectionLink m_link = HyfixCorrectionLink::Bluetooth;
	int m_nmea_interval_ms = HyfixProtocol::kDefaultNmeaIntervalMs;
	HyfixProtocol::NavigationMode m_navigation_mode = HyfixProtocol::NavigationMode::Fitness;

	QByteArray m_line_buffer;
	QList<PendingCommand> m_command_queue;
	QTimer m_command_timer;
	bool m_bring_up_started = false;
	bool m_expected = false;

	QByteArray m_correction_queue;
	QTimer m_correction_timer;
	qint64 m_dropped_bytes = 0;

	static constexpr int kMaxLineLength = 512;
};


}  // namespace OpenOrienteering

#endif
