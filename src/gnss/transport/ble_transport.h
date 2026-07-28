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


#ifndef OPENORIENTEERING_BLE_TRANSPORT_H
#define OPENORIENTEERING_BLE_TRANSPORT_H

#include <memory>

#include <QBluetoothAddress>
#include <QBluetoothUuid>
#include <QLowEnergyController>
#include <QLowEnergyService>

#include "ble_constants.h"
#include "gnss_transport.h"

namespace OpenOrienteering {


/// BLE transport using Qt Bluetooth (QLowEnergyController).
///
/// Works on macOS, Android, Linux, and Windows where Qt Bluetooth is available.
/// On iOS, CoreBluetooth direct implementation is preferred (see
/// BleTransportCoreBluetooth) but this implementation also works on iOS
/// through Qt's CoreBluetooth backend.
///
/// Connects to a BLE peripheral advertising the Nordic UART Service (NUS),
/// subscribes to the TX characteristic for incoming GNSS data, and writes
/// RTCM correction bytes to the RX characteristic.
class BleTransport : public GnssTransport
{
	Q_OBJECT

public:
	/// Create a BLE transport that will connect to the given device.
	/// \param deviceAddress BLE device address (MAC on Android/desktop, UUID on iOS/macOS)
	/// \param deviceName Human-readable device name for display
	BleTransport(const QBluetoothAddress& deviceAddress,
	             const QString& deviceName,
	             QObject* parent = nullptr);

	/// Create from a device UUID (iOS/macOS style).
	BleTransport(const QBluetoothUuid& deviceUuid,
	             const QString& deviceName,
	             QObject* parent = nullptr);

	~BleTransport() override;

	void connectToDevice() override;
	void disconnectFromDevice() override;
	bool write(const QByteArray& data) override;
	State state() const override;
	QString typeName() const override;
	QString deviceName() const override;

	/// Set custom service/characteristic UUIDs (defaults to NUS).
	void setServiceUuid(const QBluetoothUuid& uuid);
	void setRxCharUuid(const QBluetoothUuid& uuid);
	void setTxCharUuid(const QBluetoothUuid& uuid);

private slots:
	void onControllerConnected();
	void onControllerDisconnected();
	void onControllerError(QLowEnergyController::Error error);

	void onServiceDiscovered(const QBluetoothUuid& uuid);
	void onServiceDiscoveryFinished();

	void onServiceStateChanged(QLowEnergyService::ServiceState state);
	void onCharacteristicChanged(const QLowEnergyCharacteristic& characteristic,
	                             const QByteArray& value);
	void onCharacteristicWritten(const QLowEnergyCharacteristic& characteristic,
	                             const QByteArray& value);
	void onServiceError(QLowEnergyService::ServiceError error);

private:
	void setupController();
	void subscribeToTx();
	void setState(State newState);

	std::unique_ptr<QLowEnergyController> m_controller;
	QLowEnergyService* m_nusService = nullptr;  // owned by controller
	QLowEnergyCharacteristic m_rxChar;  // write (phone → receiver)
	QLowEnergyCharacteristic m_txChar;  // notify (receiver → phone)

	QBluetoothAddress m_deviceAddress;
	QBluetoothUuid m_deviceUuid;
	QString m_deviceName;
	bool m_useUuid = false;  // true on iOS/macOS where we use UUID not MAC

	QBluetoothUuid m_serviceUuid = BleGnss::kNusServiceUuid;
	QBluetoothUuid m_rxCharUuid  = BleGnss::kNusRxCharUuid;
	QBluetoothUuid m_txCharUuid  = BleGnss::kNusTxCharUuid;

	State m_state = State::Disconnected;
};


}  // namespace OpenOrienteering

#endif
