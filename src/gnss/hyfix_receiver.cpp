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

#include "hyfix_receiver.h"

#include <algorithm>

#include <QLatin1String>

namespace OpenOrienteering {

namespace {

/// Settling delay after the transport reports a connection, before the first
/// command. The vendor app waits this long on BLE; a serial link does not need
/// it, but paying it once costs nothing next to a survey session.
constexpr int kConnectSettleMs = 800;

/// Delay between handshake steps. The receiver answers a query before the next
/// one is due, so this only keeps the MCU's command parser from being flooded.
constexpr int kCommandSpacingMs = 500;

/// Extra settling before the navigation-mode command: the preceding rate
/// change restarts the GNSS engine, and a command sent mid-restart is lost.
constexpr int kNavigationModeSettleMs = 2000;

}  // namespace


HyfixReceiver::HyfixReceiver(QObject* parent)
    : QObject(parent)
{
	m_command_timer.setSingleShot(true);
	connect(&m_command_timer, &QTimer::timeout, this, &HyfixReceiver::sendNextCommand);

	m_correction_timer.setSingleShot(false);
	m_correction_timer.setInterval(HyfixProtocol::kCorrectionChunkIntervalMs);
	connect(&m_correction_timer, &QTimer::timeout, this, &HyfixReceiver::drainCorrections);

	m_release_fallback_timer.setSingleShot(true);
	m_release_fallback_timer.setInterval(kCorrectionReleaseFallbackMs);
	connect(&m_release_fallback_timer, &QTimer::timeout, this, [this]() {
		if (m_corrections_released)
			return;
		emit errorOccurred(tr("Starting corrections without a work mode acknowledgement from the receiver"));
		releaseCorrections();
	});
}


HyfixReceiver::~HyfixReceiver() = default;


HyfixCorrectionLink HyfixReceiver::linkForTransport(const QString& transport_type)
{
	const auto normalized = transport_type.trimmed().toUpper();
	if (normalized.contains(QLatin1String("SERIAL")) || normalized.contains(QLatin1String("USB")))
		return HyfixCorrectionLink::UsbC;
	// No Mapper transport reaches a GEO-PULSE over TCP, and the WIFI token is
	// unverified against firmware (its native network path reports NTRIPCLI);
	// the mapping exists only so an unexpected transport type is visible.
	if (normalized.contains(QLatin1String("TCP")))
		return HyfixCorrectionLink::Wifi;
	// BLE and Bluetooth SPP both terminate on the receiver's Bluetooth stack.
	return HyfixCorrectionLink::Bluetooth;
}


void HyfixReceiver::setCorrectionLink(HyfixCorrectionLink link)
{
	m_link = link;
}


void HyfixReceiver::setNmeaIntervalMs(int interval_ms)
{
	m_nmea_interval_ms = HyfixProtocol::nearestSupportedNmeaIntervalMs(interval_ms);
}


void HyfixReceiver::begin()
{
	m_command_queue.clear();
	m_line_buffer.clear();
	m_bring_up_started = false;
	m_info = {};

	m_corrections_released = false;
	m_release_fallback_timer.stop();
	// begin() opens a session (fresh connection or reconnect), so the
	// once-per-session USB warning re-arms here.
	m_usb_warning_emitted = false;

	enqueueCommand(kConnectSettleMs, HyfixProtocol::queryVersion());
	m_command_timer.start(m_command_queue.first().delay_ms);
}


void HyfixReceiver::startBringUp()
{
	if (m_bring_up_started)
		return;
	m_bring_up_started = true;

	// Tell the receiver where its corrections will come from, then learn the
	// rest of its identity and configuration, set the position rate, and read
	// the configuration back so the UI shows what the receiver actually did
	// rather than what we asked for.
	enqueueCommand(kCommandSpacingMs, HyfixProtocol::setRoverMode(m_link));
	enqueueCommand(kCommandSpacingMs, HyfixProtocol::queryGnssVersion());
	enqueueCommand(kCommandSpacingMs, HyfixProtocol::querySerialNumber());
	enqueueCommand(kCommandSpacingMs, HyfixProtocol::queryWorkMode());
	enqueueCommand(kCommandSpacingMs, HyfixProtocol::queryAntenna());
	enqueueCommand(kCommandSpacingMs, HyfixProtocol::setNmeaIntervalMs(m_nmea_interval_ms));
	enqueueCommand(kCommandSpacingMs, HyfixProtocol::queryMessageConfig());
	enqueueCommand(kCommandSpacingMs, HyfixProtocol::queryNtripClientStatus());
	// After the rate change's GNSS engine restart has settled: enable the
	// receiver's own position error estimate, then the dynamic model for
	// foot survey. Dead reckoning is deliberately left alone — this firmware
	// rejects $PQTMCFGDR with "unsupported command" (two-wheel builds cannot
	// disable DR), and uncalibrated DR is inert: it never engages without
	// sustained vehicle motion.
	enqueueCommand(kNavigationModeSettleMs,
	               HyfixProtocol::enableEstimatedPositionError());
	enqueueCommand(kCommandSpacingMs,
	               HyfixProtocol::setNavigationMode(m_navigation_mode));

	if (!m_command_timer.isActive() && !m_command_queue.isEmpty())
		m_command_timer.start(m_command_queue.first().delay_ms);
}


void HyfixReceiver::stop()
{
	m_command_timer.stop();
	m_correction_timer.stop();
	m_release_fallback_timer.stop();
	m_command_queue.clear();
	m_correction_queue.clear();
	m_line_buffer.clear();
	m_bring_up_started = false;
	m_corrections_released = false;
}


void HyfixReceiver::enqueueCommand(int delay_ms, const QByteArray& command)
{
	m_command_queue.append({delay_ms, command});
}


void HyfixReceiver::sendNextCommand()
{
	if (m_command_queue.isEmpty())
		return;

	const auto step = m_command_queue.takeFirst();
	emit writeRequested(step.command);

	if (!m_command_queue.isEmpty())
		m_command_timer.start(m_command_queue.first().delay_ms);
}


void HyfixReceiver::handleIncomingData(const QByteArray& data)
{
	// RTCM and other binary records share this stream, and a 0x2b frame byte
	// looks like the '+' that opens a reply. The accumulator therefore
	// resynchronizes on the `+HYFIX,` prefix: any byte that breaks the
	// prefix discards the accumulation, and while the prefix is still being
	// matched a fresh '+' restarts it — so binary noise in front of a real
	// reply cannot hide it. Once the full prefix has matched, a '+' is
	// content: the SN reply's Base64 ciphertext legitimately contains one.
	static const char kReplyPrefix[] = "+HYFIX,";
	constexpr int kReplyPrefixLength = int(sizeof(kReplyPrefix)) - 1;

	for (char c : data)
	{
		if (c == '\n')
		{
			if (!m_line_buffer.isEmpty())
			{
				if (m_line_buffer.endsWith('\r'))
					m_line_buffer.chop(1);
				handleLine(m_line_buffer);
				m_line_buffer.clear();
			}
			continue;
		}

		const auto in_prefix = m_line_buffer.size() < kReplyPrefixLength;
		if (c == '+' && in_prefix)
		{
			m_line_buffer.clear();
			m_line_buffer.append(c);
			continue;
		}

		if (m_line_buffer.isEmpty())
			continue;

		m_line_buffer.append(c);
		if (m_line_buffer.size() <= kReplyPrefixLength
		    && !std::equal(m_line_buffer.cbegin(), m_line_buffer.cend(), kReplyPrefix))
		{
			m_line_buffer.clear();
		}
		else if (m_line_buffer.size() > kMaxLineLength)
		{
			m_line_buffer.clear();
		}
	}
}


void HyfixReceiver::handleLine(const QByteArray& line)
{
	HyfixReply reply;
	if (!HyfixProtocol::parseReply(line, reply))
		return;

	const auto previous_error = m_info.lastError;
	if (HyfixProtocol::applyReply(reply, m_info))
		emit infoChanged(m_info);

	if (!m_info.lastError.isEmpty() && m_info.lastError != previous_error)
		emit errorOccurred(m_info.lastError);

	// The only bench-proven RTK bring-up delivered no RTCM before the
	// receiver had acknowledged the work mode naming the host link, so that
	// reply is what releases the correction hold-off: either the `OK`
	// acknowledgement of our own WORKMODE command, or a readback confirming
	// the full expected state — ROVER, NTRIPCLI, and the host link. A
	// partial match (e.g. a BASE mode that happens to name the same link)
	// keeps the hold.
	if (!m_corrections_released && reply.verb == QLatin1String("WORKMODE"))
	{
		const auto acknowledged = !reply.fields.isEmpty()
		                          && reply.fields.first() == QLatin1String("OK");
		const auto readback_matches = reply.fields.size() >= 3
		    && reply.fields.at(0) == QLatin1String("ROVER")
		    && reply.fields.at(1) == QLatin1String("NTRIPCLI")
		    && reply.fields.at(2) == HyfixProtocol::linkToken(m_link);
		if (acknowledged || readback_matches)
			releaseCorrections();
	}

	// The first reply of any kind confirms a GEO-PULSE is on the far end.
	startBringUp();
}


void HyfixReceiver::setDeadReckoningState(int calibration_state, int navigation_type)
{
	bool changed = false;
	if (calibration_state >= 0 && m_info.drCalibrationState != calibration_state)
	{
		m_info.drCalibrationState = calibration_state;
		changed = true;
	}
	if (navigation_type >= 0 && m_info.drNavType != navigation_type)
	{
		m_info.drNavType = navigation_type;
		changed = true;
	}
	if (changed)
		emit infoChanged(m_info);
}


void HyfixReceiver::sendCorrections(const QByteArray& rtcm)
{
	if (rtcm.isEmpty())
		return;

	if (m_link == HyfixCorrectionLink::UsbC && !m_usb_warning_emitted)
	{
		// Bench-proven on GPv2 3.8.2: with a USB data connection attached the
		// receiver was never observed to reach an RTK solution, while the
		// same receiver and corrections work over Bluetooth with power-only
		// USB. USB corrections stay wired up as experimental, but the user
		// gets told once per session rather than debugging a silent failure.
		m_usb_warning_emitted = true;
		emit errorOccurred(tr("USB corrections are experimental on this receiver: "
		                      "it has not been observed to reach RTK while a USB data "
		                      "connection is attached. Prefer Bluetooth for corrections."));
	}

	m_correction_queue.append(rtcm);
	if (m_correction_queue.size() > kMaxQueuedCorrectionBytes)
	{
		const auto excess = m_correction_queue.size() - kMaxQueuedCorrectionBytes;
		m_correction_queue.remove(0, int(excess));
		m_dropped_bytes += excess;
	}

	if (!m_corrections_released)
	{
		// Held until the WORKMODE acknowledgement; the fallback keeps a
		// non-acknowledging device from holding corrections forever.
		if (!m_release_fallback_timer.isActive())
			m_release_fallback_timer.start();
		return;
	}

	if (!pacesCorrections())
	{
		drainCorrections();
	}
	else if (!m_correction_timer.isActive())
	{
		// Send the first chunk immediately; the timer meters the rest.
		drainCorrections();
		if (!m_correction_queue.isEmpty())
			m_correction_timer.start();
	}
}


void HyfixReceiver::releaseCorrections()
{
	m_corrections_released = true;
	m_release_fallback_timer.stop();

	if (m_correction_queue.isEmpty())
		return;

	if (!pacesCorrections())
	{
		drainCorrections();
	}
	else if (!m_correction_timer.isActive())
	{
		drainCorrections();
		if (!m_correction_queue.isEmpty())
			m_correction_timer.start();
	}
}


void HyfixReceiver::drainCorrections()
{
	if (m_correction_queue.isEmpty() || !m_corrections_released)
	{
		m_correction_timer.stop();
		return;
	}

	// A metered link goes out one vendor-sized chunk at a time; any other
	// link takes the whole backlog in one write.
	const auto chunk_size = pacesCorrections()
	    ? std::min<qsizetype>(HyfixProtocol::kCorrectionChunkBytes, m_correction_queue.size())
	    : m_correction_queue.size();
	emit correctionWriteRequested(m_correction_queue.left(int(chunk_size)));
	m_correction_queue.remove(0, int(chunk_size));

	if (m_correction_queue.isEmpty())
		m_correction_timer.stop();
}


}  // namespace OpenOrienteering
