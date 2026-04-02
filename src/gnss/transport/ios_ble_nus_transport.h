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


#ifndef OPENORIENTEERING_IOS_BLE_NUS_TRANSPORT_H
#define OPENORIENTEERING_IOS_BLE_NUS_TRANSPORT_H

#include "gnss_transport.h"

#ifdef __OBJC__
@class IosBleNusDelegate;
@class CBCentralManager;
@class CBPeripheral;
#else
using IosBleNusDelegate = void;
using CBCentralManager = void;
using CBPeripheral = void;
#endif

namespace OpenOrienteering {


/// BLE transport using native CoreBluetooth, NUS (Nordic UART Service) only.
///
/// Owns the full BLE connection lifecycle: connect, service/characteristic
/// discovery, notification subscription, data transfer, disconnect, and
/// automatic reconnection after transient disconnects.
///
/// Constructed from a CBCentralManager + CBPeripheral handed off by
/// BleDiscoveryAgent. The transport takes ownership of both objects.
///
/// Internal reconnect behavior:
///   - Transient disconnect: re-calls connectPeripheral: (CB queues indefinitely)
///   - Bluetooth power cycle: retrieves peripheral by saved UUID after PoweredOn
///   - Only reports Disconnected on explicit user disconnect
///
/// The GnssSession's existing reconnect timer serves as a cold-reconnect
/// fallback if the transport reports Disconnected.
class IosBleNusTransport : public GnssTransport
{
	Q_OBJECT

public:
	/// Construct from a discovery handoff.
	/// Takes ownership of the CBCentralManager and CBPeripheral.
	/// Only called from .mm files (BleDiscoveryAgent::createTransportForDevice).
	IosBleNusTransport(CBCentralManager* manager,
	                   CBPeripheral* peripheral,
	                   const QString& peripheralUuid,
	                   const QString& deviceName,
	                   QObject* parent = nullptr);

	~IosBleNusTransport() override;

	void connectToDevice() override;
	void disconnectFromDevice() override;
	bool write(const QByteArray& data) override;
	State state() const override;
	QString typeName() const override;
	QString deviceName() const override;

	/// Negotiated MTU (payload bytes). 0 if not yet negotiated.
	int negotiatedMtu() const { return m_negotiatedMtu; }

	// ---- Called by ObjC delegate ----
	void didUpdateState(int centralState);
	void didConnectPeripheral();
	void didDisconnectPeripheral(const QString& error);
	void didFailToConnect(const QString& error);
	void didDiscoverNusService();
	void didDiscoverNusCharacteristics(bool rxFound, bool txFound);
	void didSubscribeToTx(int mtu);
	void didReceiveData(const QByteArray& data);
	void didWriteData(int bytesWritten);

private:
	void setState(State newState);
	void attemptReconnect();

	IosBleNusDelegate* m_delegate = nullptr;
	QString m_peripheralUuid;
	QString m_deviceName;
	State m_state = State::Disconnected;
	int m_negotiatedMtu = 0;
	bool m_intentionalDisconnect = false;
	bool m_wasConnected = false;  // true after first successful connection
};


}  // namespace OpenOrienteering

#endif
