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
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimeZone>

#include "correction/ntrip_client.h"
#include "gga_generator.h"
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
	resetSessionState();

	m_reconnectTimer.setSingleShot(true);
	connect(&m_reconnectTimer, &QTimer::timeout,
	        this, &GnssSession::onReconnectTimer);

	if (auto* guiApp = qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
	{
		connect(guiApp, &QGuiApplication::applicationStateChanged,
		        this, [this](Qt::ApplicationState state) {
			if (state == Qt::ApplicationActive)
				handleForegroundResume();
		});
	}
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
	connect(m_ubxParser.get(), &UbxParser::positionObservation,
	        this, &GnssSession::onPositionObservation);
	connect(m_ubxParser.get(), &UbxParser::dopObservation,
	        this, &GnssSession::onDopObservation);
	connect(m_ubxParser.get(), &UbxParser::satelliteObservation,
	        this, &GnssSession::onSatelliteObservation);
	connect(m_ubxParser.get(), &UbxParser::covarianceObservation,
	        this, &GnssSession::onCovarianceObservation);
	connect(m_ubxParser.get(), &UbxParser::statusObservation,
	        this, &GnssSession::onStatusObservation);
	connect(m_ubxParser.get(), &UbxParser::versionObservation,
	        this, &GnssSession::onVersionObservation);

	// NMEA signals
	connect(m_nmeaParser.get(), &NmeaParser::positionObservation,
	        this, &GnssSession::onPositionObservation);
	connect(m_nmeaParser.get(), &NmeaParser::dopObservation,
	        this, &GnssSession::onDopObservation);
	connect(m_nmeaParser.get(), &NmeaParser::satelliteObservation,
	        this, &GnssSession::onSatelliteObservation);

	// RTCM framer signals
	connect(m_rtcmFramer.get(), &RtcmFramer::frameValidated,
	        this, &GnssSession::onRtcmFrameValidated);
}


void GnssSession::setTransport(std::unique_ptr<GnssTransport> transport)
{
	if (m_transport)
		stop();

	m_transport = std::move(transport);
	resetSessionState();
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
	m_state.correctionState = m_ntrip ? GnssCorrectionState::Disconnected
	                                  : GnssCorrectionState::Disabled;
	if (m_ntrip)
	{
		connectNtripSignals();
		m_state.ntripProfileName = m_ntrip->mountpoint();
	}
}


void GnssSession::start()
{
	if (!m_transport)
		return;

	m_intentionalStop = false;
	m_reconnectBackoffMs = 0;
	m_reconnectTimer.stop();
	resetSessionState();
	m_state.sessionStart = QDateTime::currentDateTimeUtc();
	m_state.transportType = m_transport->typeName();
	m_state.deviceName = m_transport->deviceName();

	m_transport->connectToDevice();

	// If transport is already connected (e.g., BLE handoff already established
	// the connection), configure immediately — the stateChanged signal won't fire.
	if (m_transport->state() == GnssTransport::State::Connected)
	{
		m_state.transportState = GnssTransportState::Connected;
		configureReceiver();
		if (m_ntrip && m_ntrip->state() == NtripClient::State::Disconnected)
			startNtrip();
		emitStateChanged();
	}
}


void GnssSession::stop()
{
	m_intentionalStop = true;
	m_reconnectTimer.stop();

	if (m_transport)
		m_transport->disconnectFromDevice();

	if (m_rawLogger)
		m_rawLogger->stop();

	resetSessionState();
	m_state.transportState = GnssTransportState::Disconnected;
	m_state.transportType = m_transport ? m_transport->typeName() : QString{};
	m_state.deviceName = m_transport ? m_transport->deviceName() : QString{};
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
	connect(m_ntrip.get(), &NtripClient::correctionDataReceived,
	        this, &GnssSession::onNtripCorrectionData);
	connect(m_ntrip.get(), &NtripClient::stateChanged,
	        this, [this](NtripClient::State /*newState*/) { onNtripStateChanged(); });
	connect(m_ntrip.get(), &NtripClient::errorOccurred,
	        this, [this](const QString& msg) {
		emit errorOccurred(QStringLiteral("NTRIP"), msg);
	});
}


// ---- Transport callbacks ----


void GnssSession::onTransportDataReceived(const QByteArray& data)
{
	appendRawEntry('R', data);

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

			// Always feed both parsers — each skips bytes it doesn't recognize.
			// The receiver may switch protocols mid-stream (e.g., NMEA → UBX
			// after we send configuration), so single-parser routing is fragile.
			m_ubxParser->addData(m_detectionBuffer);
			m_nmeaParser->addData(m_detectionBuffer);
			m_detectionBuffer.clear();
		}
		return;
	}

	// Always feed both parsers — each skips foreign bytes at its sync level.
	// Protocol detection is informational (for UI display), not routing.
	m_ubxParser->addData(data);
	m_nmeaParser->addData(data);
}


void GnssSession::onTransportStateChanged(GnssTransport::State transportState)
{
	switch (transportState) {
	case GnssTransport::State::Disconnected:
		resetSessionState();
		m_state.transportState = GnssTransportState::Disconnected;
		m_state.transportType = m_transport ? m_transport->typeName() : QString{};
		m_state.deviceName = m_transport ? m_transport->deviceName() : QString{};
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
		configureReceiver();
		// Auto-start NTRIP when transport connects
		if (m_ntrip && m_ntrip->state() == NtripClient::State::Disconnected)
			startNtrip();
		break;
	case GnssTransport::State::Reconnecting:
		m_state.transportState = GnssTransportState::Reconnecting;
		m_reconnectTimer.stop();  // transport is handling reconnect internally
		break;
	}
	emitStateChanged();
}


void GnssSession::onTransportError(const QString& message)
{
	qWarning("GNSS transport error: %s", qPrintable(message));
	emit errorOccurred(QStringLiteral("Transport"), message);
	resetSessionState();
	m_state.transportState = GnssTransportState::Disconnected;
	m_state.transportType = m_transport ? m_transport->typeName() : QString{};
	m_state.deviceName = m_transport ? m_transport->deviceName() : QString{};
	emitStateChanged();
}


// ---- Parser callbacks ----


void GnssSession::recordMessage(const QString& name)
{
	auto now = QDateTime::currentMSecsSinceEpoch();
	for (int i = 0; i < m_state.messageStatCount; ++i)
	{
		if (m_state.messageStats[i].name == name)
		{
			auto& s = m_state.messageStats[i];
			s.count++;
			if (s.lastTimeMs > 0)
			{
				float dt = (now - s.lastTimeMs) * 0.001f;
				if (dt > 0.0f)
				{
					float instantHz = 1.0f / dt;
					// Exponential moving average
					s.avgHz = (s.count <= 2) ? instantHz : s.avgHz * 0.8f + instantHz * 0.2f;
				}
			}
			s.lastTimeMs = now;
			return;
		}
	}
	// New message type
	if (m_state.messageStatCount < GnssState::kMaxMessageStats)
	{
		auto& s = m_state.messageStats[m_state.messageStatCount++];
		s.name = name;
		s.count = 1;
		s.lastTimeMs = now;
		s.avgHz = 0.0f;
	}
}


void GnssSession::onPositionObservation(const GnssPositionObservation& observation)
{
	switch (observation.meta.source) {
	case GnssObservationSource::UbxNavPvt:
		recordMessage(QStringLiteral("UBX NAV-PVT"));
		break;
	case GnssObservationSource::NmeaGga:
	case GnssObservationSource::NmeaRmc:
		recordMessage(gnssObservationSourceName(observation.meta.source));
		break;
	default:
		recordMessage(gnssObservationSourceName(observation.meta.source));
		break;
	}

	m_fusion.ingest(observation);
	updateSolution();
	if (m_state.solution.hasFreshPosition)
		emit positionUpdated(m_state.solution.position);
	emit solutionUpdated(m_state.solution);
	emitStateChanged();
}


void GnssSession::onDopObservation(const GnssDopObservation& observation)
{
	recordMessage(gnssObservationSourceName(observation.meta.source));
	m_fusion.ingest(observation);
	updateSolution();
	emit solutionUpdated(m_state.solution);
	emitStateChanged();
}


void GnssSession::onSatelliteObservation(const GnssSatelliteObservation& observation)
{
	recordMessage(gnssObservationSourceName(observation.meta.source));
	m_fusion.ingest(observation);
	updateSolution();
	emit solutionUpdated(m_state.solution);
	emitStateChanged();
}


void GnssSession::onCovarianceObservation(const GnssCovarianceObservation& observation)
{
	recordMessage(gnssObservationSourceName(observation.meta.source));
	m_fusion.ingest(observation);
	updateSolution();
	emit solutionUpdated(m_state.solution);
	emitStateChanged();
}


void GnssSession::onStatusObservation(const GnssStatusObservation& observation)
{
	recordMessage(gnssObservationSourceName(observation.meta.source));
	m_fusion.ingest(observation);
	updateSolution();
	emit solutionUpdated(m_state.solution);
	emitStateChanged();
}


void GnssSession::onVersionObservation(const GnssVersionObservation& observation)
{
	m_state.receiverSwVersion = observation.swVersion;
	m_state.receiverHwVersion = observation.hwVersion;

	for (const auto& ext : observation.extensions)
	{
		if (ext.startsWith(QLatin1String("MOD=")))
		{
			m_state.receiverModel = ext.mid(4);
			break;
		}
	}

	emitStateChanged();
}


// ---- NTRIP callbacks ----


void GnssSession::onNtripCorrectionData(const QByteArray& data)
{
	// Raw logging
	if (m_rawLogger && m_rawLogger->isLogging())
		m_rawLogger->logCorrectionData(data);

	// Validate the RTCM data
	m_rtcmFramer->addData(data);

	appendRawEntry('C', data);

	// Forward corrections to the receiver
	if (m_transport && m_transport->state() == GnssTransport::State::Connected)
	{
		appendRawEntry('T', data);
		m_transport->write(data);
		if (m_ntrip)
			m_ntrip->addBytesSentToReceiver(data.size());
	}

	m_state.lastCorrectionTime = QDateTime::currentDateTimeUtc();

	// Update live metrics from NTRIP client
	if (m_ntrip)
	{
		m_state.correctionDataRate = m_ntrip->dataRate();
		m_state.localCorrectionAge = m_ntrip->correctionAge();
		m_state.ggaSentCount = m_ntrip->ggaSentCount();
		m_state.ntripBytesReceived = m_ntrip->totalBytesReceived();
		m_state.ntripBytesSentToReceiver = m_ntrip->totalBytesSentToReceiver();
		m_state.ntripVersion = m_ntrip->negotiatedVersion();
		m_state.ntripServer = m_ntrip->serverString();
	}
	emitStateChanged();
}


void GnssSession::onNtripStateChanged()
{
	if (!m_ntrip)
		return;

	switch (m_ntrip->state()) {
	case NtripClient::State::Disconnected:
		m_state.correctionState = GnssCorrectionState::Disconnected;
		break;
	case NtripClient::State::Connecting:
		m_state.correctionState = GnssCorrectionState::Connecting;
		break;
	case NtripClient::State::Connected:
		m_state.correctionState = GnssCorrectionState::Connected;
		break;
	case NtripClient::State::Flowing:
		m_state.correctionState = GnssCorrectionState::Flowing;
		break;
	case NtripClient::State::Stale:
		m_state.correctionState = GnssCorrectionState::Stale;
		break;
	case NtripClient::State::Reconnecting:
		m_state.correctionState = GnssCorrectionState::Reconnecting;
		break;
	}

	m_state.correctionDataRate = m_ntrip->dataRate();
	m_state.ntripMountpoint = m_ntrip->mountpoint();
	m_state.reconnectCount = m_ntrip->reconnectCount();
	m_state.ntripVersion = m_ntrip->negotiatedVersion();
	m_state.ntripServer = m_ntrip->serverString();
	m_state.ggaSentCount = m_ntrip->ggaSentCount();
	m_state.ntripBytesReceived = m_ntrip->totalBytesReceived();
	m_state.ntripBytesSentToReceiver = m_ntrip->totalBytesSentToReceiver();
	emitStateChanged();
}


void GnssSession::onRtcmFrameValidated(int messageType, int payloadLength)
{
	recordMessage(QStringLiteral("RTCM %1").arg(messageType));
	Q_UNUSED(messageType)
	Q_UNUSED(payloadLength)
	// Track correction health metrics
	m_state.correctionState = GnssCorrectionState::Flowing;
}


void GnssSession::resetSessionState()
{
	auto transportType = m_state.transportType;
	auto deviceName = m_state.deviceName;
	auto sessionStart = m_state.sessionStart;

	m_state = {};
	m_state.transportType = transportType;
	m_state.deviceName = deviceName;
	m_state.sessionStart = sessionStart;
	m_state.correctionState = m_ntrip ? GnssCorrectionState::Disconnected
	                                  : GnssCorrectionState::Disabled;
	m_state.ntripProfileName = m_ntrip ? m_ntrip->mountpoint() : QString{};

	m_protocolDetected = false;
	m_detectedProtocol = GnssProtocol::Unknown;
	m_detectionBuffer.clear();
	m_fusion.reset();
	m_ubxParser->reset();
	m_nmeaParser->reset();
	m_rtcmFramer->reset();
	m_rawRing.clear();
	m_rawRingBytes = 0;
}


void GnssSession::updateNtripGga()
{
	if (!m_ntrip || !m_state.solution.position.valid)
		return;

	auto gga = GgaGenerator::fromPosition(m_state.solution.position);
	if (!gga.isEmpty())
		m_ntrip->setGgaSentence(gga);
}


void GnssSession::updateSolution()
{
	m_state.solution = m_fusion.solution();
	if (m_state.solution.hasFreshPosition && m_state.solution.positionSource.observedAt.isValid())
		m_state.lastPositionTime = m_state.solution.positionSource.observedAt;
	updateNtripGga();
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


void GnssSession::setRawCaptureEnabled(bool enable)
{
	if (m_rawCaptureEnabled == enable)
		return;

	m_rawCaptureEnabled = enable;
	m_rawRing.clear();
	m_rawRingBytes = 0;
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

	auto sessionStart = m_state.sessionStart;
	resetSessionState();
	m_state.sessionStart = sessionStart;
	m_state.transportType = m_transport->typeName();
	m_state.deviceName = m_transport->deviceName();

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


void GnssSession::startNtrip()
{
	if (!m_ntrip)
		return;

	// Generate initial GGA from current position (or empty if no fix yet)
	updateNtripGga();

	m_state.correctionState = GnssCorrectionState::Connecting;
	m_state.ntripProfileName = m_ntrip->mountpoint();
	emitStateChanged();

	m_ntrip->start();
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

	// Send UBX configuration to enable richer data (covariance, satellite
	// info, etc.). Both parsers always receive all data, so even if the
	// receiver starts in NMEA mode and switches to UBX after config, the
	// UBX parser picks it up seamlessly.
	auto commands = UbxConfig::buildInitSequence();
	for (const auto& cmd : commands)
	{
		appendRawEntry('W', cmd);  // W = config write to receiver
		m_transport->write(cmd);
	}
}


void GnssSession::appendRawEntry(char direction, const QByteArray& data)
{
	if (!m_rawCaptureEnabled)
		return;

	m_rawRing.append({QDateTime::currentMSecsSinceEpoch(), data, direction});
	m_rawRingBytes += data.size();

	// Trim old entries to stay under budget
	while (m_rawRingBytes > kRawRingMaxBytes && !m_rawRing.isEmpty())
	{
		m_rawRingBytes -= m_rawRing.first().data.size();
		m_rawRing.removeFirst();
	}
}


QString GnssSession::dumpRawBuffer() const
{
	if (m_rawRing.isEmpty())
		return {};

	// Use temp directory — always writable on iOS
	auto docsPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	if (docsPath.isEmpty())
		docsPath = QDir::tempPath();

	auto timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
	auto filePath = docsPath + QStringLiteral("/gnss-raw-%1.bin").arg(timestamp);

	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly))
	{
		// Return the error as the path so the UI can show what went wrong
		return QStringLiteral("ERR: %1 (%2)").arg(file.errorString(), filePath);
	}

	// Write a simple header + entries
	// Format: each entry is [timestamp_ms:8][direction:1][length:4][data:N]
	// Header: magic + entry count
	file.write("GNSSRAW1");  // magic
	qint32 count = static_cast<qint32>(m_rawRing.size());
	file.write(reinterpret_cast<const char*>(&count), 4);

	// Also write a human-readable index alongside
	QFile indexFile(filePath + QStringLiteral(".txt"));
	auto indexOpenOk = indexFile.open(QIODevice::WriteOnly | QIODevice::Text);
	Q_UNUSED(indexOpenOk)
	QTextStream idx(&indexFile);
	idx << "GNSS Raw Dump: " << timestamp << "\n";
	idx << "Entries: " << count << "\n";
	idx << "Directions: R=receiver→app, C=NTRIP→app, T=app→receiver, W=config→receiver\n\n";

	qint64 baseTime = m_rawRing.first().timestampMs;
	for (const auto& entry : m_rawRing)
	{
		// Binary entry
		file.write(reinterpret_cast<const char*>(&entry.timestampMs), 8);
		file.write(&entry.direction, 1);
		qint32 len = static_cast<qint32>(entry.data.size());
		file.write(reinterpret_cast<const char*>(&len), 4);
		file.write(entry.data);

		// Text index line
		float relTime = (entry.timestampMs - baseTime) * 0.001f;
		idx << QString::number(static_cast<double>(relTime), 'f', 3) << "s  "
		    << entry.direction << "  " << len << " bytes";

		// For small entries or text data, show content preview
		if (len <= 80 && entry.direction != 'C' && entry.direction != 'T')
		{
			// Check if it's mostly printable (NMEA)
			bool printable = true;
			for (int i = 0; i < qMin(len, 40); ++i)
			{
				auto c = entry.data[i];
				if (c < 0x20 && c != '\r' && c != '\n')
				{
					printable = false;
					break;
				}
			}
			if (printable)
				idx << "  " << QString::fromLatin1(entry.data).trimmed();
			else
				idx << "  [hex] " << entry.data.left(20).toHex(' ');
		}
		else if (entry.direction == 'W')
		{
			idx << "  [hex] " << entry.data.left(20).toHex(' ');
		}
		idx << "\n";
	}

	file.close();
	indexFile.close();

	qDebug("GNSS: dumped %d raw entries (%lld bytes) to %s",
	       count, m_rawRingBytes, qPrintable(filePath));
	return filePath;
}


}  // namespace OpenOrienteering
