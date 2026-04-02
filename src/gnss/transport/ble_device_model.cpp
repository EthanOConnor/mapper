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

#include "ble_device_model.h"

namespace OpenOrienteering {


BleDeviceModel::BleDeviceModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

BleDeviceModel::~BleDeviceModel() = default;


int BleDeviceModel::rowCount(const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : int(m_devices.size());
}


QVariant BleDeviceModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || index.row() >= m_devices.size())
		return {};

	const auto& device = m_devices[index.row()];
	switch (role) {
	case NameRole:    return device.name;
	case AddressRole: return device.address;
	case RssiRole:    return device.rssi;
	case IsKnownRole: return device.isKnown;
	default:          return {};
	}
}


QHash<int, QByteArray> BleDeviceModel::roleNames() const
{
	return {
		{NameRole,    "name"},
		{AddressRole, "address"},
		{RssiRole,    "rssi"},
		{IsKnownRole, "isKnown"},
	};
}


void BleDeviceModel::addOrUpdate(const BleDeviceInfo& device)
{
	int idx = findByAddress(device.address);
	if (idx >= 0)
	{
		m_devices[idx].rssi = device.rssi;
		if (!device.name.isEmpty())
			m_devices[idx].name = device.name;
		emit dataChanged(index(idx), index(idx));
	}
	else
	{
		beginInsertRows({}, m_devices.size(), m_devices.size());
		m_devices.append(device);
		endInsertRows();
	}
}


void BleDeviceModel::clearDiscovered()
{
	for (int i = m_devices.size() - 1; i >= 0; --i)
	{
		if (!m_devices[i].isKnown)
		{
			beginRemoveRows({}, i, i);
			m_devices.removeAt(i);
			endRemoveRows();
		}
	}
}


void BleDeviceModel::markAsKnown(const QString& address)
{
	int idx = findByAddress(address);
	if (idx >= 0)
	{
		m_devices[idx].isKnown = true;
		emit dataChanged(index(idx), index(idx));
	}
}


const BleDeviceInfo& BleDeviceModel::deviceAt(int index) const
{
	return m_devices[index];
}


int BleDeviceModel::findByAddress(const QString& address) const
{
	for (int i = 0; i < m_devices.size(); ++i)
	{
		if (m_devices[i].address == address)
			return i;
	}
	return -1;
}


}  // namespace OpenOrienteering
