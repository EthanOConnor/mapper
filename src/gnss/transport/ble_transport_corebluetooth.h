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


#ifndef OPENORIENTEERING_BLE_TRANSPORT_COREBLUETOOTH_H
#define OPENORIENTEERING_BLE_TRANSPORT_COREBLUETOOTH_H

#include "gnss_transport.h"

// Forward-declare the Objective-C delegate/implementation class.
// The actual CBCentralManager/CBPeripheral work lives in the .mm file.
#ifdef __OBJC__
@class BleCoreBtDelegate;
#else
using BleCoreBtDelegate = void;
#endif

namespace OpenOrienteering {


/// BLE transport using native CoreBluetooth (iOS/macOS).
///
/// Provides direct access to CoreBluetooth APIs for:
///   - Background mode with state restoration (bluetooth-central)
///   - MTU negotiation (request 512, expect 128+ on F9P)
///   - Write-with/without-response selection for RTCM throughput
///   - CBCentralManager state restoration for BLE continuity
///
/// Connects to a BLE peripheral advertising the Nordic UART Service (NUS).
/// Subscribes to the TX characteristic for incoming GNSS data and writes
/// RTCM correction bytes to the RX characteristic.
class BleTransportCoreBluetooth : public GnssTransport
{
	Q_OBJECT

public:
	/// Create a transport that will connect to the given peripheral UUID.
	/// On iOS, BLE devices are identified by a per-app UUID, not MAC address.
	BleTransportCoreBluetooth(const QString& peripheralUuidString,
	                          const QString& deviceName,
	                          QObject* parent = nullptr);

	~BleTransportCoreBluetooth() override;

	void connectToDevice() override;
	void disconnectFromDevice() override;
	bool write(const QByteArray& data) override;
	State state() const override;
	QString typeName() const override;
	QString deviceName() const override;

	/// Negotiated MTU (minus 3 for ATT header). 0 if not yet negotiated.
	int negotiatedMtu() const { return m_negotiatedMtu; }

	// --- Called by the ObjC delegate ---
	void didUpdateState(int centralState);
	void didConnectPeripheral();
	void didDisconnectPeripheral(const QString& error);
	void didFailToConnect(const QString& error);
	void didDiscoverServices();
	void didDiscoverCharacteristics();
	void didUpdateNotificationState(bool enabled);
	void didReceiveData(const QByteArray& data);
	void didWriteData(int bytesWritten);
	void didNegotiateMtu(int mtu);

private:
	void setState(State newState);

	BleCoreBtDelegate* m_delegate = nullptr;  // ObjC delegate, strong ref
	QString m_peripheralUuid;
	QString m_deviceName;
	State m_state = State::Disconnected;
	int m_negotiatedMtu = 0;
};


}  // namespace OpenOrienteering

#endif
