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

#include "ntrip_client.h"

#include <QDateTime>
#include <QRandomGenerator>

namespace OpenOrienteering {


NtripClient::NtripClient(QObject* parent)
    : QObject(parent)
{
	// Staleness timer: fires every second to check for data freshness
	m_stalenessTimer.setInterval(1000);
	connect(&m_stalenessTimer, &QTimer::timeout,
	        this, &NtripClient::onStalenessTimerTimeout);

	// GGA injection timer
	connect(&m_ggaTimer, &QTimer::timeout,
	        this, &NtripClient::onGgaTimerTimeout);

	// Reconnect timer (single-shot, variable delay)
	m_reconnectTimer.setSingleShot(true);
	connect(&m_reconnectTimer, &QTimer::timeout,
	        this, &NtripClient::onReconnectTimerTimeout);
}


NtripClient::~NtripClient()
{
	stop();
}


void NtripClient::setProfile(const NtripProfile& profile)
{
	m_profile = profile;
}


void NtripClient::start()
{
	if (m_state != State::Disconnected)
		stop();

	m_reconnectCount = 0;
	m_currentBackoffMs = 0;
	m_headersParsed = false;
	m_headerBuffer.clear();
	m_dataRate = 0.0f;
	m_bytesInWindow = 0;
	m_v2Failed = false;
	m_triedV2 = false;
	m_chunkedTransfer = false;
	m_chunkBuffer.clear();
	m_chunkRemaining = 0;
	m_ggaSentCount = 0;
	m_totalBytesReceived = 0;
	m_totalBytesSent = 0;
	m_negotiatedVersion.clear();
	m_serverString.clear();

	attemptConnect();
}


void NtripClient::stop()
{
	m_stalenessTimer.stop();
	m_ggaTimer.stop();
	m_reconnectTimer.stop();

	if (m_socket)
	{
		m_socket->disconnect(this);
		m_socket->abort();
		m_socket->deleteLater();
		m_socket = nullptr;
	}

	m_headersParsed = false;
	m_headerBuffer.clear();
	setState(State::Disconnected);
}


void NtripClient::setGgaSentence(const QByteArray& gga)
{
	m_currentGga = gga;
}


float NtripClient::correctionAge() const
{
	if (m_lastDataTimestamp == 0)
		return -1.0f;
	auto now = QDateTime::currentMSecsSinceEpoch();
	return (now - m_lastDataTimestamp) / 1000.0f;
}


void NtripClient::attemptConnect()
{
	if (m_profile.casterHost.isEmpty() || m_profile.mountpoint.isEmpty())
	{
		qWarning("NTRIP: profile incomplete (host='%s' mount='%s')",
		         qPrintable(m_profile.casterHost), qPrintable(m_profile.mountpoint));
		emit errorOccurred(QStringLiteral("NTRIP profile incomplete"));
		return;
	}

	qDebug("NTRIP: connecting to %s:%d/%s (attempt %d)",
	       qPrintable(m_profile.casterHost), m_profile.casterPort,
	       qPrintable(m_profile.mountpoint), m_reconnectCount + 1);

	setState(m_reconnectCount > 0 ? State::Reconnecting : State::Connecting);

	if (m_socket)
	{
		m_socket->disconnect(this);  // disconnect all signals before deletion
		m_socket->abort();
		m_socket->deleteLater();
		m_socket = nullptr;
	}

	m_socket = new QTcpSocket(this);
	m_headersParsed = false;
	m_headerBuffer.clear();

	// Aggressive TCP keepalive for mobile
	m_socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
	m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

	connect(m_socket, &QTcpSocket::connected,
	        this, &NtripClient::onSocketConnected);
	connect(m_socket, &QTcpSocket::readyRead,
	        this, &NtripClient::onSocketReadyRead);
	connect(m_socket, &QTcpSocket::disconnected,
	        this, &NtripClient::onSocketDisconnected);
	connect(m_socket, &QTcpSocket::errorOccurred,
	        this, &NtripClient::onSocketError);

	m_socket->connectToHost(m_profile.casterHost, m_profile.casterPort);
}


void NtripClient::onSocketConnected()
{
	sendNtripRequest();
}


void NtripClient::sendNtripRequest()
{
	// Decide whether to use v2 or v1
	bool useV2 = false;
	switch (m_profile.version) {
	case NtripVersion::Auto:
		useV2 = !m_v2Failed;  // Try v2 first, fall back to v1
		break;
	case NtripVersion::V1:
		useV2 = false;
		break;
	case NtripVersion::V2:
		useV2 = true;
		break;
	}
	m_triedV2 = useV2;

	QByteArray request;
	request.append("GET /");
	request.append(m_profile.mountpoint.toLatin1());

	if (useV2)
	{
		// NTRIP v2: HTTP/1.1 with Ntrip-Version: Ntrip/2.0
		request.append(" HTTP/1.1\r\n");
		request.append("Host: ");
		request.append(m_profile.casterHost.toLatin1());
		request.append("\r\n");
		request.append("Ntrip-Version: Ntrip/2.0\r\n");
	}
	else
	{
		// NTRIP v1: HTTP/1.0 with Ntrip-Version: Ntrip/1.0
		request.append(" HTTP/1.0\r\n");
		request.append("Ntrip-Version: Ntrip/1.0\r\n");
	}

	request.append("User-Agent: NTRIP OpenOrienteeringMapper/1.0\r\n");

	if (!m_profile.username.isEmpty())
	{
		QByteArray credentials = m_profile.username.toLatin1()
		    + ':' + m_profile.password.toLatin1();
		request.append("Authorization: Basic ");
		request.append(credentials.toBase64());
		request.append("\r\n");
	}

	request.append("\r\n");

	qDebug("NTRIP: sending request (%d bytes) to %s:%d",
	       request.size(), qPrintable(m_profile.casterHost), m_profile.casterPort);
	m_socket->write(request);

	setState(State::Connected);
	m_stalenessTimer.start();
	m_lastDataTimestamp = QDateTime::currentMSecsSinceEpoch();
	m_windowStartTime = m_lastDataTimestamp;
	m_bytesInWindow = 0;

	// Start GGA injection if configured
	if (m_profile.sendGga && m_profile.ggaIntervalSec > 0)
	{
		m_ggaTimer.start(m_profile.ggaIntervalSec * 1000);
		// Send initial GGA immediately (critical for VRS on reconnect)
		sendGga();
	}
}


void NtripClient::onSocketReadyRead()
{
	if (!m_socket)
		return;

	QByteArray data = m_socket->readAll();
	if (data.isEmpty())
		return;

	if (!m_headersParsed)
	{
		// Parse HTTP response headers
		m_headerBuffer.append(data);
		int headerEnd = m_headerBuffer.indexOf("\r\n\r\n");
		if (headerEnd < 0)
		{
			// Headers not complete yet; wait for more data
			if (m_headerBuffer.size() > 4096)
			{
				emit errorOccurred(QStringLiteral("NTRIP response headers too large"));
				scheduleReconnect();
			}
			return;
		}

		// Check response status
		auto headerStr = QString::fromLatin1(m_headerBuffer.left(headerEnd));
		// SOURCETABLE 200 OK means the mountpoint was not found — the caster
		// returns the sourcetable instead of a data stream.
		bool isSourcetable = headerStr.startsWith(QLatin1String("SOURCETABLE 200 OK"));
		bool accepted = !isSourcetable
		    && (headerStr.startsWith(QLatin1String("ICY 200 OK"))
		        || headerStr.startsWith(QLatin1String("HTTP/1.0 200"))
		        || headerStr.startsWith(QLatin1String("HTTP/1.1 200")));

		if (!accepted)
		{
			if (isSourcetable)
			{
				qWarning("NTRIP: mountpoint '%s' not found — caster returned sourcetable",
				         qPrintable(m_profile.mountpoint));
				emit errorOccurred(QStringLiteral("Mountpoint \"%1\" not found on caster. Check spelling.")
				    .arg(m_profile.mountpoint));
				// Don't reconnect for a bad mountpoint — it won't fix itself
				setState(State::Disconnected);
				if (m_socket)
				{
					m_socket->disconnect(this);
					m_socket->abort();
					m_socket->deleteLater();
					m_socket = nullptr;
				}
				return;
			}

			qWarning("NTRIP: caster rejected request. Response: %s",
			         qPrintable(headerStr.left(120)));

			// If we tried v2 in Auto mode and it was rejected, fall back to v1
			if (m_triedV2 && m_profile.version == NtripVersion::Auto && !m_v2Failed)
			{
				m_v2Failed = true;
				qDebug("NTRIP: v2 rejected, falling back to v1");
				if (m_socket)
				{
					m_socket->disconnect(this);
					m_socket->abort();
					m_socket->deleteLater();
					m_socket = nullptr;
				}
				m_headersParsed = false;
				m_headerBuffer.clear();
				attemptConnect();
				return;
			}

			emit errorOccurred(QStringLiteral("NTRIP caster rejected: ") + headerStr.left(80));
			scheduleReconnect();
			return;
		}

		m_headersParsed = true;
		parseResponseHeaders(headerStr);

		// Extract any data after the headers
		data = m_headerBuffer.mid(headerEnd + 4);
		m_headerBuffer.clear();

		if (data.isEmpty())
			return;
	}

	// De-chunk if needed (NTRIP v2 chunked transfer)
	if (m_chunkedTransfer)
		data = dechunkData(data);

	if (data.isEmpty())
		return;

	// RTCM correction data
	m_totalBytesReceived += data.size();
	m_lastDataTimestamp = QDateTime::currentMSecsSinceEpoch();
	updateDataRate(data.size());

	if (m_state != State::Flowing)
		setState(State::Flowing);

	// Reset backoff on successful data receipt
	m_currentBackoffMs = 0;
	m_reconnectCount = 0;

	emit correctionDataReceived(data);
}


void NtripClient::onSocketDisconnected()
{
	m_stalenessTimer.stop();
	m_ggaTimer.stop();

	if (m_state != State::Disconnected)
		scheduleReconnect();
}


void NtripClient::onSocketError(QAbstractSocket::SocketError error)
{
	Q_UNUSED(error)

	QString msg = m_socket ? m_socket->errorString() : QStringLiteral("Socket error");
	qWarning("NTRIP: socket error: %s", qPrintable(msg));
	emit errorOccurred(msg);

	if (m_state != State::Disconnected)
		scheduleReconnect();
}


void NtripClient::onStalenessTimerTimeout()
{
	if (m_lastDataTimestamp == 0)
		return;

	auto now = QDateTime::currentMSecsSinceEpoch();
	auto elapsed = now - m_lastDataTimestamp;

	if (elapsed >= kReconnectTriggerMs && m_state != State::Reconnecting)
	{
		// Stale for too long — force reconnect
		scheduleReconnect();
	}
	else if (elapsed >= kStaleTimeoutMs && m_state == State::Flowing)
	{
		// Mark as stale but don't reconnect yet
		setState(State::Stale);
	}
}


void NtripClient::onGgaTimerTimeout()
{
	sendGga();
}


void NtripClient::onReconnectTimerTimeout()
{
	attemptConnect();
}


void NtripClient::sendGga()
{
	if (!m_socket || m_currentGga.isEmpty())
		return;

	if (m_socket->state() == QAbstractSocket::ConnectedState)
	{
		m_socket->write(m_currentGga);
		++m_ggaSentCount;
	}
}


void NtripClient::scheduleReconnect()
{
	m_stalenessTimer.stop();
	m_ggaTimer.stop();

	if (m_socket)
	{
		m_socket->disconnect(this);  // prevent stale signal delivery
		m_socket->abort();
		m_socket->deleteLater();
		m_socket = nullptr;
	}

	++m_reconnectCount;

	// Exponential backoff with ±20% jitter
	if (m_currentBackoffMs == 0)
	{
		m_currentBackoffMs = 0;  // Immediate first retry
	}
	else
	{
		m_currentBackoffMs = qMin(m_currentBackoffMs * 2, kMaxBackoffMs);
	}

	int delay = m_currentBackoffMs;
	if (delay > 0)
	{
		// Add ±20% jitter
		int jitter = QRandomGenerator::global()->bounded(delay * 2 / 5) - (delay / 5);
		delay = qMax(0, delay + jitter);
	}

	if (m_currentBackoffMs == 0)
		m_currentBackoffMs = 1000;  // Set to 1s for next retry

	setState(State::Reconnecting);

	if (delay == 0)
		attemptConnect();
	else
		m_reconnectTimer.start(delay);
}


void NtripClient::updateDataRate(int bytesReceived)
{
	auto now = QDateTime::currentMSecsSinceEpoch();
	m_bytesInWindow += bytesReceived;

	auto windowElapsed = now - m_windowStartTime;
	if (windowElapsed >= kRateWindowMs)
	{
		m_dataRate = static_cast<float>(m_bytesInWindow) * 1000.0f
		    / static_cast<float>(windowElapsed);
		m_bytesInWindow = 0;
		m_windowStartTime = now;
	}
}


void NtripClient::setState(State newState)
{
	if (m_state != newState)
	{
		m_state = newState;
		emit stateChanged(m_state);
	}
}


void NtripClient::parseResponseHeaders(const QString& headers)
{
	// Detect chunked transfer encoding (NTRIP v2)
	m_chunkedTransfer = headers.contains(
	    QLatin1String("Transfer-Encoding: chunked"), Qt::CaseInsensitive);
	m_chunkBuffer.clear();
	m_chunkRemaining = 0;

	// Determine negotiated version
	if (headers.startsWith(QLatin1String("ICY 200 OK")))
		m_negotiatedVersion = QStringLiteral("v1");
	else if (m_chunkedTransfer)
		m_negotiatedVersion = QStringLiteral("v2 chunked");
	else if (m_triedV2)
		m_negotiatedVersion = QStringLiteral("v2");
	else
		m_negotiatedVersion = QStringLiteral("v1");

	// Extract Server: header
	m_serverString.clear();
	for (const auto& line : headers.split(QLatin1String("\r\n")))
	{
		if (line.startsWith(QLatin1String("Server:"), Qt::CaseInsensitive))
		{
			m_serverString = line.mid(7).trimmed();
			break;
		}
	}

	qDebug("NTRIP: connected (%s), server: %s",
	       qPrintable(m_negotiatedVersion), qPrintable(m_serverString));
}


QByteArray NtripClient::dechunkData(const QByteArray& raw)
{
	// HTTP chunked transfer encoding:
	//   <hex-size>\r\n<data>\r\n<hex-size>\r\n<data>\r\n...0\r\n\r\n
	// Data may arrive split across multiple readyRead calls, so we
	// maintain m_chunkBuffer and m_chunkRemaining across calls.

	m_chunkBuffer.append(raw);
	QByteArray result;

	while (!m_chunkBuffer.isEmpty())
	{
		if (m_chunkRemaining > 0)
		{
			// Reading chunk data
			int available = qMin(m_chunkRemaining, static_cast<int>(m_chunkBuffer.size()));
			result.append(m_chunkBuffer.left(available));
			m_chunkBuffer.remove(0, available);
			m_chunkRemaining -= available;

			if (m_chunkRemaining == 0)
			{
				// Consume trailing \r\n after chunk data
				if (m_chunkBuffer.startsWith("\r\n"))
					m_chunkBuffer.remove(0, 2);
			}
		}
		else
		{
			// Reading chunk size line
			int lineEnd = m_chunkBuffer.indexOf("\r\n");
			if (lineEnd < 0)
				break;  // Incomplete size line — wait for more data

			bool ok = false;
			int chunkSize = m_chunkBuffer.left(lineEnd).trimmed().toInt(&ok, 16);
			m_chunkBuffer.remove(0, lineEnd + 2);

			if (!ok || chunkSize < 0)
			{
				qWarning("NTRIP: invalid chunk size, dropping buffer");
				m_chunkBuffer.clear();
				break;
			}

			if (chunkSize == 0)
			{
				// Terminal chunk — stream complete (caster closed)
				m_chunkBuffer.clear();
				break;
			}

			m_chunkRemaining = chunkSize;
		}
	}

	return result;
}


}  // namespace OpenOrienteering
