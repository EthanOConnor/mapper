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


#ifndef OPENORIENTEERING_BLE_DISCOVERY_AGENT_H
#define OPENORIENTEERING_BLE_DISCOVERY_AGENT_H

#include <memory>

#include <QObject>
#include <QString>

#ifdef __OBJC__
@class BleDiscoveryDelegate;
#else
using BleDiscoveryDelegate = void;
#endif

namespace OpenOrienteering {

class BleDeviceModel;
class GnssTransport;


/// Scan-only BLE discovery agent for iOS (CoreBluetooth).
///
/// Scans for peripherals advertising the Nordic UART Service (NUS) and
/// populates a BleDeviceModel for the device-picker UI.
///
/// When the user selects a device, call createTransportForDevice() to
/// produce an IosBleNusTransport that owns the BLE connection. The agent
/// transfers its CBCentralManager and the cached CBPeripheral to the
/// transport and becomes inert.
///
/// After createTransportForDevice(), the agent should be deleted. To
/// re-scan (e.g., after a cancel), create a new BleDiscoveryAgent.
class BleDiscoveryAgent : public QObject
{
	Q_OBJECT

public:
	BleDiscoveryAgent(BleDeviceModel* model, QObject* parent = nullptr);
	~BleDiscoveryAgent() override;

	void startScan();
	void stopScan();
	bool isScanning() const;

	/// Create a transport for the selected device.
	///
	/// Stops scanning, detaches the CBCentralManager from the discovery
	/// delegate, and transfers it along with the cached CBPeripheral to
	/// a new IosBleNusTransport. The agent becomes inert after this call.
	///
	/// Returns nullptr if the UUID is not found in the peripheral cache
	/// (e.g., the device went out of range since discovery).
	std::unique_ptr<GnssTransport> createTransportForDevice(
	    const QString& uuid, const QString& deviceName);

	// ---- Called by ObjC delegate ----
	void didUpdateState(int centralState);
	void didDiscoverDevice(const QString& name, const QString& uuid, int rssi);

signals:
	void scanningChanged(bool scanning);

private:
	BleDiscoveryDelegate* m_delegate = nullptr;
	BleDeviceModel* m_model = nullptr;
	bool m_scanning = false;
	bool m_scanPending = false;
};


}  // namespace OpenOrienteering

#endif
