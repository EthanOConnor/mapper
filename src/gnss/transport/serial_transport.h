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


#ifndef OPENORIENTEERING_SERIAL_TRANSPORT_H
#define OPENORIENTEERING_SERIAL_TRANSPORT_H

#include <QSerialPort>

#include "gnss_transport.h"

namespace OpenOrienteering {


/// USB serial transport for GNSS receivers connected via physical cable.
///
/// Uses QSerialPort — available on desktop platforms (macOS, Linux, Windows).
/// Not available on Android or iOS.
///
/// Default configuration: 115200 baud, 8 data bits, no parity, 1 stop bit
/// (standard for u-blox receivers). Configurable via setBaudRate().
class SerialTransport : public GnssTransport
{
	Q_OBJECT

public:
	/// Create a serial transport for the given port.
	/// \param portName System port name (e.g., "/dev/ttyACM0", "COM3")
	SerialTransport(const QString& portName, QObject* parent = nullptr);

	~SerialTransport() override;

	void connectToDevice() override;
	void disconnectFromDevice() override;
	bool write(const QByteArray& data) override;
	State state() const override;
	QString typeName() const override;
	QString deviceName() const override;

	/// Set baud rate (default 115200).
	void setBaudRate(qint32 baudRate);

	/// Current baud rate.
	qint32 baudRate() const { return m_baudRate; }

private slots:
	void onReadyRead();
	void onErrorOccurred(QSerialPort::SerialPortError error);

private:
	QSerialPort* m_port = nullptr;
	QString m_portName;
	qint32 m_baudRate = 115200;
	State m_state = State::Disconnected;
};


}  // namespace OpenOrienteering

#endif
