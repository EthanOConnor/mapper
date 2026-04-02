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


#include "serial_transport.h"


namespace OpenOrienteering {


SerialTransport::SerialTransport(const QString& portName, QObject* parent)
    : GnssTransport(parent)
    , m_portName(portName)
{
}


SerialTransport::~SerialTransport()
{
	if (m_port && m_port->isOpen())
		m_port->close();
}


void SerialTransport::connectToDevice()
{
	if (m_state == State::Connected || m_state == State::Connecting)
		return;

	if (!m_port)
	{
		m_port = new QSerialPort(this);
		connect(m_port, &QSerialPort::readyRead,
		        this, &SerialTransport::onReadyRead);
		connect(m_port, &QSerialPort::errorOccurred,
		        this, &SerialTransport::onErrorOccurred);
	}

	m_port->setPortName(m_portName);
	m_port->setBaudRate(m_baudRate);
	m_port->setDataBits(QSerialPort::Data8);
	m_port->setParity(QSerialPort::NoParity);
	m_port->setStopBits(QSerialPort::OneStop);
	m_port->setFlowControl(QSerialPort::NoFlowControl);

	m_state = State::Connecting;
	emit stateChanged(m_state);

	if (m_port->open(QIODevice::ReadWrite))
	{
		m_state = State::Connected;
		emit stateChanged(m_state);
	}
	else
	{
		emit errorOccurred(m_port->errorString());
		m_state = State::Disconnected;
		emit stateChanged(m_state);
	}
}


void SerialTransport::disconnectFromDevice()
{
	if (m_port)
	{
		m_port->close();
		m_port->deleteLater();
		m_port = nullptr;
	}

	if (m_state != State::Disconnected)
	{
		m_state = State::Disconnected;
		emit stateChanged(m_state);
	}
}


bool SerialTransport::write(const QByteArray& data)
{
	if (!m_port || m_state != State::Connected)
		return false;

	auto written = m_port->write(data);
	if (written > 0)
		emit writeComplete(static_cast<int>(written));
	return written == data.size();
}


GnssTransport::State SerialTransport::state() const
{
	return m_state;
}


QString SerialTransport::typeName() const
{
	return QStringLiteral("Serial");
}


QString SerialTransport::deviceName() const
{
	return m_portName + QLatin1String(" @ ") + QString::number(m_baudRate);
}


void SerialTransport::setBaudRate(qint32 baudRate)
{
	m_baudRate = baudRate;
	if (m_port && m_port->isOpen())
		m_port->setBaudRate(baudRate);
}


void SerialTransport::onReadyRead()
{
	if (m_port)
		emit dataReceived(m_port->readAll());
}


void SerialTransport::onErrorOccurred(QSerialPort::SerialPortError error)
{
	if (error == QSerialPort::NoError)
		return;

	QString message;
	if (m_port)
		message = m_port->errorString();

	// Resource error typically means the device was unplugged
	if (error == QSerialPort::ResourceError)
	{
		m_state = State::Disconnected;
		emit stateChanged(m_state);
	}

	if (!message.isEmpty())
		emit errorOccurred(message);
}


}  // namespace OpenOrienteering
