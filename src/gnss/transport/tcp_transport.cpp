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

#include "tcp_transport.h"

#include <QTcpSocket>

#ifdef Q_OS_UNIX
#  include <sys/socket.h>
#  include <netinet/tcp.h>
#  include <netinet/in.h>
#endif
#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif

namespace OpenOrienteering {


TcpTransport::TcpTransport(const QHostAddress& host, quint16 port, QObject* parent)
    : GnssTransport(parent)
    , m_host(host)
    , m_port(port)
{
	m_connectTimer.setSingleShot(true);
	connect(&m_connectTimer, &QTimer::timeout,
	        this, &TcpTransport::onConnectTimeout);
}


TcpTransport::~TcpTransport()
{
	if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState)
		m_socket->abort();
}


void TcpTransport::connectToDevice()
{
	if (m_state == State::Connected || m_state == State::Connecting)
		return;

	if (!m_socket)
	{
		m_socket = new QTcpSocket(this);
		connect(m_socket, &QTcpSocket::connected, this, &TcpTransport::onConnected);
		connect(m_socket, &QTcpSocket::disconnected, this, &TcpTransport::onDisconnected);
		connect(m_socket, &QTcpSocket::readyRead, this, &TcpTransport::onReadyRead);
		connect(m_socket, &QTcpSocket::errorOccurred, this, &TcpTransport::onErrorOccurred);
	}

	m_state = State::Connecting;
	emit stateChanged(m_state);

	m_connectTimer.start(kConnectTimeoutMs);
	m_socket->connectToHost(m_host, m_port);
}


void TcpTransport::disconnectFromDevice()
{
	if (m_socket)
	{
		m_socket->abort();
		m_socket->deleteLater();
		m_socket = nullptr;
	}

	if (m_state != State::Disconnected)
	{
		m_state = State::Disconnected;
		emit stateChanged(m_state);
	}
}


bool TcpTransport::write(const QByteArray& data)
{
	if (!m_socket || m_state != State::Connected)
		return false;

	auto written = m_socket->write(data);
	if (written > 0)
		emit writeComplete(static_cast<int>(written));
	return written == data.size();
}


GnssTransport::State TcpTransport::state() const
{
	return m_state;
}


QString TcpTransport::typeName() const
{
	return QStringLiteral("TCP");
}


QString TcpTransport::deviceName() const
{
	return m_host.toString() + QLatin1Char(':') + QString::number(m_port);
}


void TcpTransport::setHost(const QHostAddress& host, quint16 port)
{
	m_host = host;
	m_port = port;
}


void TcpTransport::onConnected()
{
	m_connectTimer.stop();
	enableTcpKeepalive();

	m_state = State::Connected;
	emit stateChanged(m_state);
}


void TcpTransport::onDisconnected()
{
	m_connectTimer.stop();
	m_state = State::Disconnected;
	emit stateChanged(m_state);
}


void TcpTransport::onReadyRead()
{
	if (m_socket)
		emit dataReceived(m_socket->readAll());
}


void TcpTransport::onErrorOccurred()
{
	m_connectTimer.stop();
	if (m_socket)
		emit errorOccurred(m_socket->errorString());
}


void TcpTransport::onConnectTimeout()
{
	if (m_state == State::Connecting && m_socket)
	{
		m_socket->abort();
		emit errorOccurred(QStringLiteral("Connection timed out"));
	}
}


void TcpTransport::enableTcpKeepalive()
{
#if defined(Q_OS_UNIX) || defined(Q_OS_WIN)
	if (!m_socket)
		return;

	auto fd = m_socket->socketDescriptor();
	if (fd < 0)
		return;

	int enable = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&enable), sizeof(enable));

#if defined(Q_OS_LINUX)
	int idle = 10;    // seconds before first probe
	int interval = 5; // seconds between probes
	int count = 3;    // number of probes before dropping
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#elif defined(Q_OS_MACOS) || defined(Q_OS_IOS)
	int idle = 10;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#endif

	// TCP_NODELAY for responsive GGA injection
	int nodelay = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
#endif
}


}  // namespace OpenOrienteering
