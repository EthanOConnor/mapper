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


#include "spp_transport.h"

#include <QBluetoothUuid>


namespace OpenOrienteering {


const QBluetoothUuid SppTransport::kSppServiceUuid{
	QBluetoothUuid::ServiceClassUuid::SerialPort
};


SppTransport::SppTransport(const QBluetoothAddress& deviceAddress,
                           const QString& deviceName,
                           QObject* parent)
    : GnssTransport(parent)
    , m_deviceAddress(deviceAddress)
    , m_deviceName(deviceName)
{
}


SppTransport::~SppTransport()
{
	if (m_socket && m_socket->state() != QBluetoothSocket::SocketState::UnconnectedState)
		m_socket->abort();
}


void SppTransport::connectToDevice()
{
	if (m_state == State::Connected || m_state == State::Connecting)
		return;

	if (!m_socket)
	{
		m_socket = new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol, this);
		connect(m_socket, &QBluetoothSocket::connected,
		        this, &SppTransport::onConnected);
		connect(m_socket, &QBluetoothSocket::disconnected,
		        this, &SppTransport::onDisconnected);
		connect(m_socket, &QBluetoothSocket::readyRead,
		        this, &SppTransport::onReadyRead);
		connect(m_socket, &QBluetoothSocket::errorOccurred,
		        this, &SppTransport::onErrorOccurred);
	}

	m_state = State::Connecting;
	emit stateChanged(m_state);

	m_socket->connectToService(m_deviceAddress, kSppServiceUuid);
}


void SppTransport::disconnectFromDevice()
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


bool SppTransport::write(const QByteArray& data)
{
	if (!m_socket || m_state != State::Connected)
		return false;

	auto written = m_socket->write(data);
	if (written > 0)
		emit writeComplete(static_cast<int>(written));
	return written == data.size();
}


GnssTransport::State SppTransport::state() const
{
	return m_state;
}


QString SppTransport::typeName() const
{
	return QStringLiteral("SPP");
}


QString SppTransport::deviceName() const
{
	return m_deviceName;
}


void SppTransport::onConnected()
{
	m_state = State::Connected;
	emit stateChanged(m_state);
}


void SppTransport::onDisconnected()
{
	m_state = State::Disconnected;
	emit stateChanged(m_state);
}


void SppTransport::onReadyRead()
{
	if (m_socket)
		emit dataReceived(m_socket->readAll());
}


void SppTransport::onErrorOccurred(QBluetoothSocket::SocketError error)
{
	Q_UNUSED(error)
	if (m_socket)
		emit errorOccurred(m_socket->errorString());
}


}  // namespace OpenOrienteering
