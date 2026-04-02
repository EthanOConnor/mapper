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

#include "gnss_session.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QTimeZone>

#include "correction/ntrip_client.h"
#include "gnss_raw_logger.h"
#include "protocol/ubx_config.h"
#include "protocol/ubx_parser.h"
#include "protocol/nmea_parser.h"
#include "protocol/protocol_detector.h"
#include "protocol/rtcm_framer.h"

namespace OpenOrienteering {


GnssSession::GnssSession(QObject* parent)
    : QObject(parent)
{
	setupParsers();

	m_reconnectTimer.setSingleShot(true);
	connect(&m_reconnectTimer, &QTimer::timeout,
	        this, &GnssSession::onReconnectTimer);
}

GnssSession::~GnssSession()
{
	stop();
}


void GnssSession::setupParsers()
{
	m_ubxParser = std::make_unique<UbxParser>(this);
	m_nmeaParser = std::make_unique<NmeaParser>(this);
	m_rtcmFramer = std::make_unique<RtcmFramer>(this);

	// UBX signals
	connect(m_ubxParser.get(), &UbxParser::positionUpdated,
	        this, &GnssSession::onUbxPositionUpdated);
	connect(m_ubxParser.get(), &UbxParser::dopUpdated,
	        this, &GnssSession::onUbxDopUpdated);
	connect(m_ubxParser.get(), &UbxParser::satelliteInfoUpdated,
	        this, &GnssSession::onUbxSatelliteInfoUpdated);
	connect(m_ubxParser.get(), &UbxParser::covarianceUpdated,
	        this, &GnssSession::onUbxCovarianceUpdated);
	connect(m_ubxParser.get(), &UbxParser::statusUpdated,
	        this, &GnssSession::onUbxStatusUpdated);
	connect(m_ubxParser.get(), &UbxParser::versionReceived,
	        this, &GnssSession::onUbxVersionReceived);

	// NMEA signals
	connect(m_nmeaParser.get(), &NmeaParser::positionUpdated,
	        this, &GnssSession::onNmeaPositionUpdated);
	connect(m_nmeaParser.get(), &NmeaParser::dopUpdated,
	        this, &GnssSession::onNmeaDopUpdated);

	// RTCM framer signals
	connect(m_rtcmFramer.get(), &RtcmFramer::frameValidated,
	        this, &GnssSession::onRtcmFrameValidated);
}


void GnssSession::setTransport(std::unique_ptr<GnssTransport> transport)
{
	if (m_transport)
		stop();

	m_transport = std::move(transport);
	if (m_transport)
	{
		m_state.transportType = m_transport->typeName();
		m_state.deviceName = m_transport->deviceName();
		connectTransportSignals();
	}
}


void GnssSession::setNtripClient(std::unique_ptr<NtripClient> ntrip)
{
	m_ntrip = std::move(ntrip);
	if (m_ntrip)
		connectNtripSignals();
}


void GnssSession::start()
{
	if (!m_transport)
		return;

	m_intentionalStop = false;
	m_reconnectBackoffMs = 0;
	m_reconnectTimer.stop();
	m_state.sessionStart = QDateTime::currentDateTimeUtc();
	m_protocolDetected = false;
	m_detectedProtocol = GnssProtocol::Unknown;
	m_detectionBuffer.clear();
	m_ubxParser->reset();
	m_nmeaParser->reset();
	m_rtcmFramer->reset();

	m_transport->connectToDevice();
}


void GnssSession::stop()
{
	m_intentionalStop = true;
	m_reconnectTimer.stop();

	if (m_transport)
		m_transport->disconnectFromDevice();

	if (m_rawLogger)
		m_rawLogger->stop();

	m_state.transportState = GnssTransportState::Disconnected;
	m_state.correctionState = GnssCorrectionState::Disconnected;
	emitStateChanged();
}


bool GnssSession::isActive() const
{
	return m_transport
	    && (m_state.transportState == GnssTransportState::Connecting
	        || m_state.transportState == GnssTransportState::Connected
	        || m_state.transportState == GnssTransportState::Reconnecting);
}


void GnssSession::connectTransportSignals()
{
	connect(m_transport.get(), &GnssTransport::dataReceived,
	        this, &GnssSession::onTransportDataReceived);
	connect(m_transport.get(), &GnssTransport::stateChanged,
	        this, &GnssSession::onTransportStateChanged);
	connect(m_transport.get(), &GnssTransport::errorOccurred,
	        this, &GnssSession::onTransportError);
}


void GnssSession::connectNtripSignals()
{
	// NTRIP client signals will be connected here when NtripClient is implemented.
	// For now this is a placeholder.
}


// ---- Transport callbacks ----


void GnssSession::onTransportDataReceived(const QByteArray& data)
{
	// Raw logging
	if (m_rawLogger && m_rawLogger->isLogging())
		m_rawLogger->logReceiverData(data);

	if (!m_protocolDetected)
	{
		// Buffer initial data for protocol detection
		m_detectionBuffer.append(data);
		if (m_detectionBuffer.size() >= ProtocolDetector::kMinDetectionBytes)
		{
			m_detectedProtocol = ProtocolDetector::detect(m_detectionBuffer);
			m_protocolDetected = true;
			m_state.protocol = m_detectedProtocol;

			// Feed the buffered data to the appropriate parser(s)
			switch (m_detectedProtocol) {
			case GnssProtocol::UBX:
				m_ubxParser->addData(m_detectionBuffer);
				break;
			case GnssProtocol::NMEA:
				m_nmeaParser->addData(m_detectionBuffer);
				break;
			case GnssProtocol::Mixed:
				// Feed to both parsers — each will skip bytes it doesn't understand
				m_ubxParser->addData(m_detectionBuffer);
				m_nmeaParser->addData(m_detectionBuffer);
				break;
			case GnssProtocol::Unknown:
				// Try both and see what sticks
				m_ubxParser->addData(m_detectionBuffer);
				m_nmeaParser->addData(m_detectionBuffer);
				break;
			}
			m_detectionBuffer.clear();
		}
		return;
	}

	// Route data to the detected parser(s)
	switch (m_detectedProtocol) {
	case GnssProtocol::UBX:
		m_ubxParser->addData(data);
		break;
	case GnssProtocol::NMEA:
		m_nmeaParser->addData(data);
		break;
	case GnssProtocol::Mixed:
	case GnssProtocol::Unknown:
		m_ubxParser->addData(data);
		m_nmeaParser->addData(data);
		break;
	}
}


void GnssSession::onTransportStateChanged(GnssTransport::State transportState)
{
	switch (transportState) {
	case GnssTransport::State::Disconnected:
		m_state.transportState = GnssTransportState::Disconnected;
		// Auto-reconnect if not intentionally stopped
		if (m_autoReconnect && !m_intentionalStop)
			scheduleReconnect();
		break;
	case GnssTransport::State::Connecting:
		m_state.transportState = GnssTransportState::Connecting;
		break;
	case GnssTransport::State::Connected:
		m_state.transportState = GnssTransportState::Connected;
		m_reconnectBackoffMs = 0;  // Reset backoff on successful connect
		// Send UBX configuration commands
		configureReceiver();
		break;
	case GnssTransport::State::Reconnecting:
		m_state.transportState = GnssTransportState::Reconnecting;
		break;
	}
	emitStateChanged();
}


void GnssSession::onTransportError(const QString& message)
{
	qWarning("GNSS transport error: %s", qPrintable(message));
	emit errorOccurred(QStringLiteral("Transport"), message);
	m_state.transportState = GnssTransportState::Disconnected;
	emitStateChanged();
}


// ---- UBX parser callbacks ----


void GnssSession::onUbxPositionUpdated(const GnssPosition& position)
{
	m_state.position = position;
	m_state.lastPositionTime = QDateTime::currentDateTimeUtc();
	emit positionUpdated(position);
	emitStateChanged();
}


void GnssSession::onUbxDopUpdated(float gDOP, float pDOP, float tDOP,
                                  float vDOP, float hDOP, float nDOP, float eDOP)
{
	m_state.position.gDOP = gDOP;
	m_state.position.pDOP = pDOP;
	m_state.position.tDOP = tDOP;
	m_state.position.vDOP = vDOP;
	m_state.position.hDOP = hDOP;
	m_state.position.nDOP = nDOP;
	m_state.position.eDOP = eDOP;
	emitStateChanged();
}


void GnssSession::onUbxSatelliteInfoUpdated(int totalUsed, int totalVisible)
{
	m_state.position.satellitesUsed = static_cast<std::uint8_t>(totalUsed);
	m_state.position.satellitesVisible = static_cast<std::uint8_t>(totalVisible);
	emitStateChanged();
}


void GnssSession::onUbxCovarianceUpdated(float covNN, float covNE, float covEE)
{
	m_state.position.computeP95Ellipse(covNN, covNE, covEE);
	emitStateChanged();
}


void GnssSession::onUbxStatusUpdated(bool fixOK, bool diffSoln, int carrSoln, int spoofDet)
{
	Q_UNUSED(fixOK)
	Q_UNUSED(diffSoln)
	Q_UNUSED(carrSoln)
	Q_UNUSED(spoofDet)
	// Status is already captured in NAV-PVT position. This signal provides
	// additional detail that can be surfaced in diagnostics UI later.
}


void GnssSession::onUbxVersionReceived(const QString& sw, const QString& hw,
                                       const QStringList& extensions)
{
	m_state.receiverSwVersion = sw;
	m_state.receiverHwVersion = hw;

	// Try to extract model name from extension strings
	for (const auto& ext : extensions)
	{
		if (ext.startsWith(QLatin1String("MOD=")))
		{
			m_state.receiverModel = ext.mid(4);
			break;
		}
	}
	emitStateChanged();
}


// ---- NMEA parser callbacks ----


void GnssSession::onNmeaPositionUpdated(const GnssPosition& position)
{
	// If UBX is providing positions, prefer UBX (richer metadata).
	// Only use NMEA position if protocol is NMEA-only.
	if (m_detectedProtocol == GnssProtocol::NMEA)
	{
		m_state.position = position;
		m_state.lastPositionTime = QDateTime::currentDateTimeUtc();
		emit positionUpdated(position);
		emitStateChanged();
	}
}


void GnssSession::onNmeaDopUpdated(float pDOP, float hDOP, float vDOP)
{
	if (m_detectedProtocol == GnssProtocol::NMEA)
	{
		m_state.position.pDOP = pDOP;
		m_state.position.hDOP = hDOP;
		m_state.position.vDOP = vDOP;
		emitStateChanged();
	}
}


// ---- NTRIP callbacks ----


void GnssSession::onNtripCorrectionData(const QByteArray& data)
{
	// Raw logging
	if (m_rawLogger && m_rawLogger->isLogging())
		m_rawLogger->logCorrectionData(data);

	// Validate the RTCM data
	m_rtcmFramer->addData(data);

	// Forward corrections to the receiver
	if (m_transport && m_transport->state() == GnssTransport::State::Connected)
		m_transport->write(data);

	m_state.lastCorrectionTime = QDateTime::currentDateTimeUtc();
}


void GnssSession::onNtripStateChanged()
{
	// Will be implemented when NtripClient is available
	emitStateChanged();
}


void GnssSession::onRtcmFrameValidated(int messageType, int payloadLength)
{
	Q_UNUSED(messageType)
	Q_UNUSED(payloadLength)
	// Track correction health metrics
	m_state.correctionState = GnssCorrectionState::Flowing;
}


void GnssSession::emitStateChanged()
{
	emit stateChanged(m_state);
}


// ---- Raw logging ----


void GnssSession::setRawLogging(bool enable, const QString& directory)
{
	if (enable)
	{
		if (!m_rawLogger)
			m_rawLogger = std::make_unique<GnssRawLogger>(this);
		if (!m_rawLogger->isLogging())
			m_rawLogger->start(directory.isEmpty() ? QStringLiteral(".") : directory);
	}
	else if (m_rawLogger)
	{
		m_rawLogger->stop();
	}
}


// ---- Auto-reconnect ----


void GnssSession::setAutoReconnect(bool enable)
{
	m_autoReconnect = enable;
	if (!enable)
		m_reconnectTimer.stop();
}


void GnssSession::scheduleReconnect()
{
	if (!m_autoReconnect || m_intentionalStop)
		return;

	m_state.transportState = GnssTransportState::Reconnecting;
	emitStateChanged();

	// Exponential backoff with jitter
	int delay = m_reconnectBackoffMs;
	if (delay > 0)
	{
		// Add ±20% jitter
		int jitter = delay * kReconnectJitterPercent / 100;
		delay += QRandomGenerator::global()->bounded(-jitter, jitter + 1);
	}

	// Advance backoff for next attempt
	if (m_reconnectBackoffMs == 0)
		m_reconnectBackoffMs = 1000;
	else
		m_reconnectBackoffMs = qMin(m_reconnectBackoffMs * 2, kMaxReconnectBackoffMs);

	m_reconnectTimer.start(delay);
}


void GnssSession::onReconnectTimer()
{
	if (!m_transport || m_intentionalStop)
		return;

	// Reset protocol detection for the new connection
	m_protocolDetected = false;
	m_detectedProtocol = GnssProtocol::Unknown;
	m_detectionBuffer.clear();
	m_ubxParser->reset();
	m_nmeaParser->reset();

	m_transport->connectToDevice();
}


void GnssSession::feedData(const QByteArray& data)
{
	// Mark transport as connected when receiving external data
	if (m_state.transportState != GnssTransportState::Connected)
	{
		m_state.transportState = GnssTransportState::Connected;
		emitStateChanged();
	}
	onTransportDataReceived(data);
}


// ---- iOS / background handling ----


void GnssSession::handleForegroundResume()
{
	if (m_intentionalStop || !m_transport)
		return;

	// Check transport state — BLE may have disconnected in background
	if (m_transport->state() == GnssTransport::State::Disconnected)
	{
		qDebug("GNSS: transport disconnected during background, reconnecting");
		m_reconnectBackoffMs = 0;  // Immediate reconnect on foreground return
		scheduleReconnect();
	}

	// Restart NTRIP if it lapsed
	if (m_ntrip)
	{
		auto ntripState = m_state.correctionState;
		if (ntripState == GnssCorrectionState::Disconnected
		    || ntripState == GnssCorrectionState::Stale)
		{
			qDebug("GNSS: NTRIP lapsed during background, restarting");
			m_ntrip->start();
		}
	}
}


// ---- UBX configuration ----


void GnssSession::configureReceiver()
{
	if (!m_transport || m_transport->state() != GnssTransport::State::Connected)
		return;

	// Only send UBX config if we detected UBX or Mixed protocol,
	// or if protocol is still unknown (first connect).
	if (m_detectedProtocol == GnssProtocol::NMEA)
		return;

	auto commands = UbxConfig::buildInitSequence();
	for (const auto& cmd : commands)
		m_transport->write(cmd);
}


}  // namespace OpenOrienteering
