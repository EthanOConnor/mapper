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
}


HyfixReceiver::~HyfixReceiver() = default;


HyfixCorrectionLink HyfixReceiver::linkForTransport(const QString& transport_type)
{
	const auto normalized = transport_type.trimmed().toUpper();
	if (normalized.contains(QLatin1String("SERIAL")) || normalized.contains(QLatin1String("USB")))
		return HyfixCorrectionLink::UsbC;
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
	// Last, after the rate change's GNSS engine restart has settled: the
	// dynamic model for foot survey. Dead reckoning is deliberately left
	// alone — this firmware rejects $PQTMCFGDR with "unsupported command"
	// (two-wheel builds cannot disable DR), and uncalibrated DR is inert:
	// it never engages without sustained vehicle motion.
	enqueueCommand(kNavigationModeSettleMs,
	               HyfixProtocol::setNavigationMode(m_navigation_mode));

	if (!m_command_timer.isActive() && !m_command_queue.isEmpty())
		m_command_timer.start(m_command_queue.first().delay_ms);
}


void HyfixReceiver::stop()
{
	m_command_timer.stop();
	m_correction_timer.stop();
	m_command_queue.clear();
	m_correction_queue.clear();
	m_line_buffer.clear();
	m_bring_up_started = false;
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

		// RTCM and other binary records share this stream. Only text can start
		// a `+HYFIX` line, so anything else resets the accumulator instead of
		// filling it with frame bytes.
		if (m_line_buffer.isEmpty() && c != '+')
			continue;

		m_line_buffer.append(c);
		if (m_line_buffer.size() > kMaxLineLength)
			m_line_buffer.clear();
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

	if (!pacesCorrections())
	{
		emit writeRequested(rtcm);
		return;
	}

	m_correction_queue.append(rtcm);
	if (m_correction_queue.size() > kMaxQueuedCorrectionBytes)
	{
		const auto excess = m_correction_queue.size() - kMaxQueuedCorrectionBytes;
		m_correction_queue.remove(0, int(excess));
		m_dropped_bytes += excess;
	}

	if (!m_correction_timer.isActive())
	{
		// Send the first chunk immediately; the timer meters the rest.
		drainCorrections();
		if (!m_correction_queue.isEmpty())
			m_correction_timer.start();
	}
}


void HyfixReceiver::drainCorrections()
{
	if (m_correction_queue.isEmpty())
	{
		m_correction_timer.stop();
		return;
	}

	const auto chunk_size = std::min<qsizetype>(
	    HyfixProtocol::kCorrectionChunkBytes, m_correction_queue.size());
	emit writeRequested(m_correction_queue.left(int(chunk_size)));
	m_correction_queue.remove(0, int(chunk_size));

	if (m_correction_queue.isEmpty())
		m_correction_timer.stop();
}


}  // namespace OpenOrienteering
