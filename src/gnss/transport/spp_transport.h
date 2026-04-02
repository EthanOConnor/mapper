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


#ifndef OPENORIENTEERING_SPP_TRANSPORT_H
#define OPENORIENTEERING_SPP_TRANSPORT_H

#include <QBluetoothAddress>
#include <QBluetoothSocket>

#include "gnss_transport.h"

namespace OpenOrienteering {


/// Bluetooth Classic SPP (Serial Port Profile) transport for GNSS receivers.
///
/// Uses RFCOMM sockets via Qt Bluetooth. Works on Android and desktop
/// platforms. Not available on iOS (requires MFi certification).
///
/// SPP is the most common Bluetooth transport for u-blox evaluation kits
/// and many third-party GNSS receivers that predate BLE support.
class SppTransport : public GnssTransport
{
	Q_OBJECT

public:
	/// Create an SPP transport for the given Bluetooth device.
	/// \param deviceAddress Classic Bluetooth MAC address
	/// \param deviceName Human-readable device name for display
	SppTransport(const QBluetoothAddress& deviceAddress,
	             const QString& deviceName,
	             QObject* parent = nullptr);

	~SppTransport() override;

	void connectToDevice() override;
	void disconnectFromDevice() override;
	bool write(const QByteArray& data) override;
	State state() const override;
	QString typeName() const override;
	QString deviceName() const override;

private slots:
	void onConnected();
	void onDisconnected();
	void onReadyRead();
	void onErrorOccurred(QBluetoothSocket::SocketError error);

private:
	QBluetoothSocket* m_socket = nullptr;
	QBluetoothAddress m_deviceAddress;
	QString m_deviceName;
	State m_state = State::Disconnected;

	/// Standard SPP UUID: 00001101-0000-1000-8000-00805F9B34FB
	static const QBluetoothUuid kSppServiceUuid;
};


}  // namespace OpenOrienteering

#endif
