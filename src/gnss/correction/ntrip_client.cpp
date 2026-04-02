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

	attemptConnect();
}


void NtripClient::stop()
{
	m_stalenessTimer.stop();
	m_ggaTimer.stop();
	m_reconnectTimer.stop();

	if (m_socket)
	{
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
		emit errorOccurred(QStringLiteral("NTRIP profile incomplete"));
		return;
	}

	setState(m_reconnectCount > 0 ? State::Reconnecting : State::Connecting);

	if (m_socket)
	{
		m_socket->abort();
		m_socket->deleteLater();
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
	// NTRIP v1 request format (HTTP/1.0-style):
	//   GET /mountpoint HTTP/1.0
	//   User-Agent: NTRIP OpenOrienteeringMapper/1.0
	//   Authorization: Basic <base64(user:pass)>
	//   Ntrip-Version: Ntrip/1.0
	//   \r\n

	QByteArray request;
	request.append("GET /");
	request.append(m_profile.mountpoint.toLatin1());
	request.append(" HTTP/1.0\r\n");
	request.append("User-Agent: NTRIP OpenOrienteeringMapper/1.0\r\n");
	request.append("Ntrip-Version: Ntrip/1.0\r\n");

	if (!m_profile.username.isEmpty())
	{
		QByteArray credentials = m_profile.username.toLatin1()
		    + ':' + m_profile.password.toLatin1();
		request.append("Authorization: Basic ");
		request.append(credentials.toBase64());
		request.append("\r\n");
	}

	request.append("\r\n");

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
		if (!headerStr.startsWith(QLatin1String("ICY 200 OK"))
		    && !headerStr.startsWith(QLatin1String("HTTP/1.0 200"))
		    && !headerStr.startsWith(QLatin1String("HTTP/1.1 200")))
		{
			emit errorOccurred(QStringLiteral("NTRIP caster rejected: ") + headerStr.left(80));
			scheduleReconnect();
			return;
		}

		m_headersParsed = true;

		// Extract any data after the headers
		data = m_headerBuffer.mid(headerEnd + 4);
		m_headerBuffer.clear();

		if (data.isEmpty())
			return;
	}

	// RTCM correction data
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
		m_socket->write(m_currentGga);
}


void NtripClient::scheduleReconnect()
{
	m_stalenessTimer.stop();
	m_ggaTimer.stop();

	if (m_socket)
	{
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


}  // namespace OpenOrienteering
