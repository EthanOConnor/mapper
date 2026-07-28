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


#ifndef OPENORIENTEERING_TCP_TRANSPORT_H
#define OPENORIENTEERING_TCP_TRANSPORT_H

#include <QHostAddress>
#include <QTimer>

#include "gnss_transport.h"

class QTcpSocket;

namespace OpenOrienteering {


/// TCP/IP transport for GNSS receivers with Wi-Fi or network interfaces.
///
/// Also useful for development: replay recorded GNSS data from a local
/// TCP server without needing physical hardware.
///
/// Features:
///   - Connection timeout (10 seconds)
///   - TCP keepalive (idle 10s, interval 5s, probes 3)
class TcpTransport : public GnssTransport
{
	Q_OBJECT

public:
	TcpTransport(const QHostAddress& host, quint16 port, QObject* parent = nullptr);
	~TcpTransport() override;

	void connectToDevice() override;
	void disconnectFromDevice() override;
	bool write(const QByteArray& data) override;
	State state() const override;
	QString typeName() const override;
	QString deviceName() const override;

	void setHost(const QHostAddress& host, quint16 port);

private slots:
	void onConnected();
	void onDisconnected();
	void onReadyRead();
	void onErrorOccurred();
	void onConnectTimeout();

private:
	void enableTcpKeepalive();

	QTcpSocket* m_socket = nullptr;
	QHostAddress m_host;
	quint16 m_port = 0;
	State m_state = State::Disconnected;
	QTimer m_connectTimer;

	static constexpr int kConnectTimeoutMs = 10000;
};


}  // namespace OpenOrienteering

#endif
