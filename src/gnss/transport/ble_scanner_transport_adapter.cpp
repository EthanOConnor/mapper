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

#include "ble_scanner_transport_adapter.h"

#include "ble_scanner_corebluetooth.h"

namespace OpenOrienteering {


BleScannerTransportAdapter::BleScannerTransportAdapter(
    BleScannerCoreBluetooth* scanner,
    const QString& deviceUuid,
    const QString& deviceName,
    QObject* parent)
    : GnssTransport(parent)
    , m_scanner(scanner)
    , m_deviceUuid(deviceUuid)
    , m_deviceName(deviceName)
{
	// Forward data from scanner → transport dataReceived signal
	connect(m_scanner, &BleScannerCoreBluetooth::dataReceived,
	        this, &GnssTransport::dataReceived);

	// Forward write completion
	connect(m_scanner, &BleScannerCoreBluetooth::writeComplete,
	        this, &GnssTransport::writeComplete);

	// Scanner connected → transport is connected
	connect(m_scanner, &BleScannerCoreBluetooth::deviceConnected,
	        this, [this](const QString& /*name*/) {
		setState(State::Connected);
	});

	// Scanner disconnected → transport disconnected, emit error if non-empty
	connect(m_scanner, &BleScannerCoreBluetooth::deviceDisconnected,
	        this, [this](const QString& reason) {
		setState(State::Disconnected);
		if (!reason.isEmpty())
			emit errorOccurred(reason);
	});

	// Connection failed during connect attempt
	connect(m_scanner, &BleScannerCoreBluetooth::deviceConnectionFailed,
	        this, [this](const QString& error) {
		setState(State::Disconnected);
		emit errorOccurred(error);
	});

	// If the scanner is destroyed, we're disconnected
	connect(m_scanner, &QObject::destroyed, this, [this]() {
		m_scanner = nullptr;
		setState(State::Disconnected);
	});
}


void BleScannerTransportAdapter::connectToDevice()
{
	if (!m_scanner || m_state == State::Connected || m_state == State::Connecting)
		return;

	setState(State::Connecting);
	m_scanner->connectToDevice(m_deviceUuid, m_deviceName);
}


void BleScannerTransportAdapter::disconnectFromDevice()
{
	if (m_scanner)
		m_scanner->stopScan();

	setState(State::Disconnected);
}


bool BleScannerTransportAdapter::write(const QByteArray& data)
{
	if (!m_scanner || m_state != State::Connected)
		return false;
	return m_scanner->writeData(data);
}


GnssTransport::State BleScannerTransportAdapter::state() const
{
	return m_state;
}


QString BleScannerTransportAdapter::typeName() const
{
	return QStringLiteral("BLE");
}


QString BleScannerTransportAdapter::deviceName() const
{
	return m_deviceName;
}


void BleScannerTransportAdapter::setState(State newState)
{
	if (m_state != newState)
	{
		m_state = newState;
		emit stateChanged(m_state);
	}
}


}  // namespace OpenOrienteering
