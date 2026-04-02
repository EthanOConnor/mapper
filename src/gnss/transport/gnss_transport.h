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


#ifndef OPENORIENTEERING_GNSS_TRANSPORT_H
#define OPENORIENTEERING_GNSS_TRANSPORT_H

#include <QByteArray>
#include <QObject>
#include <QString>

namespace OpenOrienteering {


/// Abstract interface for GNSS receiver transport backends.
///
/// All transports (BLE, TCP, serial, etc.) implement this interface.
/// The session manager uses it to read GNSS data and write RTCM corrections
/// without knowing the underlying transport mechanism.
class GnssTransport : public QObject
{
	Q_OBJECT

public:
	enum class State
	{
		Disconnected,
		Connecting,
		Connected,
		Reconnecting,
	};
	Q_ENUM(State)

	using QObject::QObject;
	~GnssTransport() override = default;

	/// Initiate connection to the configured device.
	virtual void connectToDevice() = 0;

	/// Disconnect from the device and release resources.
	virtual void disconnectFromDevice() = 0;

	/// Write data to the device (typically RTCM corrections).
	/// Returns true if the data was queued for sending.
	virtual bool write(const QByteArray& data) = 0;

	/// Current connection state.
	virtual State state() const = 0;

	/// Human-readable transport type name (e.g., "BLE", "TCP").
	virtual QString typeName() const = 0;

	/// Human-readable device name (e.g., "u-blox ZED-F9P" or "192.168.1.100:5000").
	virtual QString deviceName() const = 0;

signals:
	/// Raw bytes received from the device (GNSS data: UBX, NMEA, etc.).
	void dataReceived(const QByteArray& data);

	/// Connection state changed.
	void stateChanged(OpenOrienteering::GnssTransport::State newState);

	/// An error occurred. The transport may attempt reconnection automatically.
	void errorOccurred(const QString& message);

	/// Write completed (all bytes sent). Useful for flow control.
	void writeComplete(int bytesWritten);
};


}  // namespace OpenOrienteering

#endif
