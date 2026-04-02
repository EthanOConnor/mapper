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


#ifndef OPENORIENTEERING_BLE_SCANNER_COREBLUETOOTH_H
#define OPENORIENTEERING_BLE_SCANNER_COREBLUETOOTH_H

#include <QObject>

#ifdef __OBJC__
@class BleScannerDelegate;
#else
using BleScannerDelegate = void;
#endif

namespace OpenOrienteering {

class BleDeviceModel;


/// CoreBluetooth-based BLE device scanner for iOS.
///
/// Scans for peripherals advertising the Nordic UART Service (NUS)
/// and populates a BleDeviceModel with discovered devices.
class BleScannerCoreBluetooth : public QObject
{
	Q_OBJECT

public:
	explicit BleScannerCoreBluetooth(BleDeviceModel* model, QObject* parent = nullptr);
	~BleScannerCoreBluetooth() override;

	void startScan();
	void stopScan();
	bool isScanning() const { return m_scanning; }
	const QString& connectedDeviceUuid() const { return m_connectUuid; }

	/// Connect to a discovered device by UUID.
	/// Stops scanning, connects, discovers NUS service, subscribes to TX.
	/// If the device advertises an L2CAP PSM, opens an L2CAP channel for
	/// higher-throughput data flow (falls back to NUS if unavailable).
	/// Emits deviceConnected() when ready for data flow.
	void connectToDevice(const QString& uuid, const QString& name);

	// Called by ObjC delegate
	void didDiscoverDevice(const QString& name, const QString& uuid, int rssi);
	void didUpdateState(int state);
	void didConnect();
	void didFailToConnect(const QString& error);
	void didDisconnect(const QString& error);
	void didRetryConnect(int attempt, int maxAttempts);
	void didBecomeReady();   // NUS service found, TX subscribed
	void didReceiveData(const QByteArray& data);
	void didWriteData(int bytesWritten);
	void didNegotiateMtu(int mtu);

signals:
	void scanningChanged(bool scanning);
	void deviceConnected(const QString& name);
	void deviceDisconnected(const QString& reason);
	void deviceConnectionFailed(const QString& error);
	void connectionRetrying(int attempt, int maxAttempts);
	void dataReceived(const QByteArray& data);
	void writeComplete(int bytesWritten);

public:
	/// Write data through the connected peripheral (RTCM → receiver).
	bool writeData(const QByteArray& data);

private:
	BleScannerDelegate* m_delegate = nullptr;
	BleDeviceModel* m_model;
	bool m_scanning = false;
	bool m_scanPending = false;
	QString m_connectedDeviceName;
	QString m_connectUuid;
	int m_negotiatedMtu = 0;
	int m_connectRetries = 0;
	static constexpr int kMaxConnectRetries = 3;
};


}  // namespace OpenOrienteering

#endif
