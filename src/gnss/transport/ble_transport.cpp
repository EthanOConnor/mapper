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

#include "ble_transport.h"

#include <QLowEnergyCharacteristic>
#include <QLowEnergyDescriptor>

namespace OpenOrienteering {


BleTransport::BleTransport(const QBluetoothAddress& deviceAddress,
                           const QString& deviceName,
                           QObject* parent)
    : GnssTransport(parent)
    , m_deviceAddress(deviceAddress)
    , m_deviceName(deviceName)
{
}


BleTransport::BleTransport(const QBluetoothUuid& deviceUuid,
                           const QString& deviceName,
                           QObject* parent)
    : GnssTransport(parent)
    , m_deviceUuid(deviceUuid)
    , m_deviceName(deviceName)
    , m_useUuid(true)
{
}


BleTransport::~BleTransport()
{
	disconnectFromDevice();
}


void BleTransport::connectToDevice()
{
	if (m_state == State::Connected || m_state == State::Connecting)
		return;

	setState(State::Connecting);

	// Create a new controller each time — QLowEnergyController is not
	// designed for reconnection reuse.
	m_nusService = nullptr;
	m_rxChar = {};
	m_txChar = {};

	setupController();
	m_controller->connectToDevice();
}


void BleTransport::disconnectFromDevice()
{
	if (m_controller)
	{
		m_controller->disconnectFromDevice();
		m_controller.reset();
	}

	m_nusService = nullptr;
	m_rxChar = {};
	m_txChar = {};

	if (m_state != State::Disconnected)
		setState(State::Disconnected);
}


bool BleTransport::write(const QByteArray& data)
{
	if (m_state != State::Connected || !m_nusService || !m_rxChar.isValid())
		return false;

	// Write to the RX characteristic (phone → receiver).
	// Use WriteWithoutResponse for throughput when the characteristic supports it,
	// otherwise fall back to WriteWithResponse.
	auto writeMode = m_rxChar.properties().testFlag(QLowEnergyCharacteristic::WriteNoResponse)
	    ? QLowEnergyService::WriteWithoutResponse
	    : QLowEnergyService::WriteWithResponse;

	// BLE has MTU limits — chunk the data if needed.
	// QLowEnergyService handles chunking internally on most platforms,
	// but we chunk explicitly to be safe with arbitrary MTU sizes.
	int mtu = m_controller ? m_controller->mtu() : BleGnss::kMinimumMtu;
	int chunkSize = qMax(mtu - 3, 20);  // ATT header is 3 bytes

	for (int offset = 0; offset < data.size(); offset += chunkSize)
	{
		auto chunk = data.mid(offset, chunkSize);
		m_nusService->writeCharacteristic(m_rxChar, chunk, writeMode);
	}

	return true;
}


GnssTransport::State BleTransport::state() const
{
	return m_state;
}


QString BleTransport::typeName() const
{
	return QStringLiteral("BLE");
}


QString BleTransport::deviceName() const
{
	return m_deviceName;
}


void BleTransport::setServiceUuid(const QBluetoothUuid& uuid) { m_serviceUuid = uuid; }
void BleTransport::setRxCharUuid(const QBluetoothUuid& uuid) { m_rxCharUuid = uuid; }
void BleTransport::setTxCharUuid(const QBluetoothUuid& uuid) { m_txCharUuid = uuid; }


void BleTransport::setupController()
{
	if (m_useUuid)
		m_controller.reset(QLowEnergyController::createCentral(QBluetoothDeviceInfo(m_deviceUuid, m_deviceName, 0)));
	else
		m_controller.reset(QLowEnergyController::createCentral(QBluetoothDeviceInfo(m_deviceAddress, m_deviceName, 0)));

	connect(m_controller.get(), &QLowEnergyController::connected,
	        this, &BleTransport::onControllerConnected);
	connect(m_controller.get(), &QLowEnergyController::disconnected,
	        this, &BleTransport::onControllerDisconnected);
	connect(m_controller.get(), &QLowEnergyController::errorOccurred,
	        this, &BleTransport::onControllerError);
	connect(m_controller.get(), &QLowEnergyController::serviceDiscovered,
	        this, &BleTransport::onServiceDiscovered);
	connect(m_controller.get(), &QLowEnergyController::discoveryFinished,
	        this, &BleTransport::onServiceDiscoveryFinished);
}


void BleTransport::onControllerConnected()
{
	// Connected at the BLE level — now discover services
	m_controller->discoverServices();
}


void BleTransport::onControllerDisconnected()
{
	m_nusService = nullptr;
	m_rxChar = {};
	m_txChar = {};
	setState(State::Disconnected);
}


void BleTransport::onControllerError(QLowEnergyController::Error error)
{
	QString msg;
	switch (error) {
	case QLowEnergyController::UnknownError:
		msg = QStringLiteral("Unknown BLE error");
		break;
	case QLowEnergyController::UnknownRemoteDeviceError:
		msg = QStringLiteral("Device not found");
		break;
	case QLowEnergyController::NetworkError:
		msg = QStringLiteral("BLE network error");
		break;
	case QLowEnergyController::InvalidBluetoothAdapterError:
		msg = QStringLiteral("Invalid Bluetooth adapter");
		break;
	case QLowEnergyController::ConnectionError:
		msg = QStringLiteral("BLE connection failed");
		break;
	case QLowEnergyController::AdvertisingError:
		msg = QStringLiteral("BLE advertising error");
		break;
	case QLowEnergyController::RemoteHostClosedError:
		msg = QStringLiteral("Device disconnected");
		break;
	case QLowEnergyController::AuthorizationError:
		msg = QStringLiteral("BLE authorization failed");
		break;
	case QLowEnergyController::MissingPermissionsError:
		msg = QStringLiteral("Missing Bluetooth permissions");
		break;
	case QLowEnergyController::RssiReadError:
		msg = QStringLiteral("RSSI read error");
		break;
	default:
		msg = QStringLiteral("BLE error %1").arg(int(error));
		break;
	}
	emit errorOccurred(msg);
}


void BleTransport::onServiceDiscovered(const QBluetoothUuid& uuid)
{
	if (uuid == m_serviceUuid)
	{
		m_nusService = m_controller->createServiceObject(uuid, this);
		if (m_nusService)
		{
			connect(m_nusService, &QLowEnergyService::stateChanged,
			        this, &BleTransport::onServiceStateChanged);
			connect(m_nusService, &QLowEnergyService::characteristicChanged,
			        this, &BleTransport::onCharacteristicChanged);
			connect(m_nusService, &QLowEnergyService::characteristicWritten,
			        this, &BleTransport::onCharacteristicWritten);
			connect(m_nusService, &QLowEnergyService::errorOccurred,
			        this, &BleTransport::onServiceError);
		}
	}
}


void BleTransport::onServiceDiscoveryFinished()
{
	if (!m_nusService)
	{
		emit errorOccurred(QStringLiteral("NUS service not found on device"));
		setState(State::Disconnected);
		return;
	}

	// Discover service details (characteristics and descriptors)
	m_nusService->discoverDetails();
}


void BleTransport::onServiceStateChanged(QLowEnergyService::ServiceState serviceState)
{
	if (serviceState == QLowEnergyService::RemoteServiceDiscovered)
	{
		// Service details discovered — find our characteristics
		m_rxChar = m_nusService->characteristic(m_rxCharUuid);
		m_txChar = m_nusService->characteristic(m_txCharUuid);

		if (!m_rxChar.isValid())
		{
			emit errorOccurred(QStringLiteral("NUS RX characteristic not found"));
			setState(State::Disconnected);
			return;
		}

		if (!m_txChar.isValid())
		{
			emit errorOccurred(QStringLiteral("NUS TX characteristic not found"));
			setState(State::Disconnected);
			return;
		}

		subscribeToTx();
	}
}


void BleTransport::subscribeToTx()
{
	// Enable notifications on the TX characteristic to receive GNSS data
	auto cccd = m_txChar.descriptor(QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
	if (!cccd.isValid())
	{
		emit errorOccurred(QStringLiteral("TX characteristic has no CCCD descriptor"));
		setState(State::Disconnected);
		return;
	}

	m_nusService->writeDescriptor(cccd, QLowEnergyCharacteristic::CCCDEnableNotification);

	// We're fully connected and subscribed
	setState(State::Connected);
}


void BleTransport::onCharacteristicChanged(const QLowEnergyCharacteristic& characteristic,
                                           const QByteArray& value)
{
	if (characteristic.uuid() == m_txCharUuid)
		emit dataReceived(value);
}


void BleTransport::onCharacteristicWritten(const QLowEnergyCharacteristic& characteristic,
                                           const QByteArray& value)
{
	if (characteristic.uuid() == m_rxCharUuid)
		emit writeComplete(value.size());
}


void BleTransport::onServiceError(QLowEnergyService::ServiceError error)
{
	QString msg;
	switch (error) {
	case QLowEnergyService::OperationError:
		msg = QStringLiteral("BLE service operation error");
		break;
	case QLowEnergyService::CharacteristicWriteError:
		msg = QStringLiteral("BLE characteristic write error");
		break;
	case QLowEnergyService::DescriptorWriteError:
		msg = QStringLiteral("BLE descriptor write error");
		break;
	case QLowEnergyService::CharacteristicReadError:
		msg = QStringLiteral("BLE characteristic read error");
		break;
	case QLowEnergyService::DescriptorReadError:
		msg = QStringLiteral("BLE descriptor read error");
		break;
	default:
		msg = QStringLiteral("BLE service error %1").arg(int(error));
		break;
	}
	emit errorOccurred(msg);
}


void BleTransport::setState(State newState)
{
	if (m_state != newState)
	{
		m_state = newState;
		emit stateChanged(m_state);
	}
}


}  // namespace OpenOrienteering
