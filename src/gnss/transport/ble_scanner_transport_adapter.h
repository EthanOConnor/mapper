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


#ifndef OPENORIENTEERING_BLE_SCANNER_TRANSPORT_ADAPTER_H
#define OPENORIENTEERING_BLE_SCANNER_TRANSPORT_ADAPTER_H

#include <QString>

#include "gnss_transport.h"

namespace OpenOrienteering {

class BleScannerCoreBluetooth;


/// Adapts a BleScannerCoreBluetooth into a GnssTransport.
///
/// The scanner manages the full CoreBluetooth lifecycle (scan, connect,
/// service discovery, L2CAP). This adapter wraps it so GnssSession can
/// treat it as a normal transport with uniform state management,
/// auto-reconnect, and bidirectional data flow.
///
/// Does not own the scanner — the scanner must outlive the adapter.
class BleScannerTransportAdapter : public GnssTransport
{
	Q_OBJECT

public:
	/// Create an adapter for the given scanner and target device.
	/// The scanner must already exist; the adapter stores the device
	/// UUID/name so it can reconnect on demand.
	BleScannerTransportAdapter(BleScannerCoreBluetooth* scanner,
	                           const QString& deviceUuid,
	                           const QString& deviceName,
	                           QObject* parent = nullptr);

	void connectToDevice() override;
	void disconnectFromDevice() override;
	bool write(const QByteArray& data) override;
	State state() const override;
	QString typeName() const override;
	QString deviceName() const override;

private:
	void setState(State newState);

	BleScannerCoreBluetooth* m_scanner;
	QString m_deviceUuid;
	QString m_deviceName;
	State m_state = State::Disconnected;
};


}  // namespace OpenOrienteering

#endif
