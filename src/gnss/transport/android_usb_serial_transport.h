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


#ifndef OPENORIENTEERING_ANDROID_USB_SERIAL_TRANSPORT_H
#define OPENORIENTEERING_ANDROID_USB_SERIAL_TRANSPORT_H

#include <QList>
#include <QString>

#include "gnss_transport.h"

namespace OpenOrienteering {


struct AndroidUsbSerialDeviceInfo
{
	QString address;
	QString name;
};


/// Android USB-host serial transport for cabled GNSS receivers.
///
/// The Android side owns USB permission, driver selection, and the read/write
/// loop. This class keeps the C++ side behind the regular GnssTransport
/// contract used by BLE and desktop serial transports.
class AndroidUsbSerialTransport : public GnssTransport
{
	Q_OBJECT

public:
	AndroidUsbSerialTransport(const QString& address,
	                          const QString& deviceName,
	                          QObject* parent = nullptr);
	~AndroidUsbSerialTransport() override;

	static QList<AndroidUsbSerialDeviceInfo> availableDevices();

	void connectToDevice() override;
	void disconnectFromDevice() override;
	bool write(const QByteArray& data) override;
	State state() const override;
	QString typeName() const override;
	QString deviceName() const override;

	void setBaudRate(qint32 baudRate);
	qint32 baudRate() const { return m_baudRate; }

	void handleDataReceived(const QByteArray& data);
	void handleStateChanged(int state);
	void handleError(const QString& message);
	void handleWriteComplete(int bytes);

private:
	qintptr nativeHandle() const;
	void setState(State newState);

	QString m_address;
	QString m_deviceName;
	qint32 m_baudRate = 115200;
	State m_state = State::Disconnected;
};


}  // namespace OpenOrienteering

#endif
