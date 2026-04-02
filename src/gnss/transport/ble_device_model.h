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


#ifndef OPENORIENTEERING_BLE_DEVICE_MODEL_H
#define OPENORIENTEERING_BLE_DEVICE_MODEL_H

#include <QAbstractListModel>
#include <QString>
#include <QList>

namespace OpenOrienteering {


/// Information about a discovered BLE device.
struct BleDeviceInfo
{
	QString name;        ///< Device name (e.g., "ArduSimple BLE" or "u-blox GNSS")
	QString address;     ///< Platform-specific address (MAC on Android, UUID on iOS)
	int rssi = -100;     ///< Signal strength in dBm
	bool isKnown = false; ///< True if this device was previously connected
};


/// List model of discovered BLE GNSS devices for use in device picker UI.
///
/// Populated by BLE scanning (via Qt Bluetooth or CoreBluetooth).
/// Exposes device name, address, signal strength, and known-device status.
class BleDeviceModel : public QAbstractListModel
{
	Q_OBJECT

public:
	enum Role
	{
		NameRole    = Qt::DisplayRole,
		AddressRole = Qt::UserRole,
		RssiRole,
		IsKnownRole,
	};

	explicit BleDeviceModel(QObject* parent = nullptr);
	~BleDeviceModel() override;

	int rowCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	QHash<int, QByteArray> roleNames() const override;

	/// Add or update a device in the model. Updates RSSI if already present.
	void addOrUpdate(const BleDeviceInfo& device);

	/// Clear all non-known devices (called when starting a new scan).
	void clearDiscovered();

	/// Mark a device as known (previously connected).
	void markAsKnown(const QString& address);

	/// Get device info by index.
	const BleDeviceInfo& deviceAt(int index) const;

	/// Find device index by address. Returns -1 if not found.
	int findByAddress(const QString& address) const;

private:
	QList<BleDeviceInfo> m_devices;
};


}  // namespace OpenOrienteering

#endif
