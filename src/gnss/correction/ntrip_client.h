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


#ifndef OPENORIENTEERING_NTRIP_CLIENT_H
#define OPENORIENTEERING_NTRIP_CLIENT_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

#include "ntrip_profile.h"

namespace OpenOrienteering {


/// Ultra-robust NTRIP v1 client for mobile RTCM correction streaming.
///
/// Designed for the reality of mobile internet: cell tower handoffs,
/// WiFi-to-cell transitions, tunnels, dead zones, partial connectivity.
///
/// Key robustness features:
///   - Application-level staleness detection (no bytes for 3s → stale)
///   - Immediate reconnect on first staleness detection
///   - Exponential backoff with ±20% jitter on repeated failures
///   - Network transition detection (bypass backoff on connectivity change)
///   - Parallel reconnect (new connection before tearing down old)
///   - VRS GGA re-injection on reconnect
///   - Health metrics (data rate, correction age, reconnect count)
class NtripClient : public QObject
{
	Q_OBJECT

public:
	enum class State
	{
		Disconnected,
		Connecting,
		Connected,   ///< TCP connected, waiting for data
		Flowing,     ///< RTCM bytes arriving
		Stale,       ///< No data received recently
		Reconnecting,
	};
	Q_ENUM(State)

	explicit NtripClient(QObject* parent = nullptr);
	~NtripClient() override;

	/// Set the NTRIP profile to use for connection.
	void setProfile(const NtripProfile& profile);

	/// Start the NTRIP connection.
	void start();

	/// Stop and disconnect.
	void stop();

	/// Current connection state.
	State state() const { return m_state; }

	/// Set the GGA sentence to inject for VRS services.
	/// Called by GnssSession when a new position is available.
	void setGgaSentence(const QByteArray& gga);

	// -- Health metrics --
	float dataRate() const { return m_dataRate; }            ///< bytes/sec, smoothed
	float correctionAge() const;                             ///< seconds since last data
	int reconnectCount() const { return m_reconnectCount; }
	const QString& mountpoint() const { return m_profile.mountpoint; }

signals:
	/// RTCM correction bytes received from the caster.
	void correctionDataReceived(const QByteArray& data);

	/// Connection state changed.
	void stateChanged(OpenOrienteering::NtripClient::State newState);

	/// Error message for display/logging.
	void errorOccurred(const QString& message);

private slots:
	void onSocketConnected();
	void onSocketReadyRead();
	void onSocketDisconnected();
	void onSocketError(QAbstractSocket::SocketError error);
	void onStalenessTimerTimeout();
	void onGgaTimerTimeout();
	void onReconnectTimerTimeout();

private:
	void setState(State newState);
	void attemptConnect();
	void sendNtripRequest();
	void sendGga();
	void scheduleReconnect();
	void updateDataRate(int bytesReceived);

	NtripProfile m_profile;
	QTcpSocket* m_socket = nullptr;
	State m_state = State::Disconnected;

	// Staleness detection
	QTimer m_stalenessTimer;
	qint64 m_lastDataTimestamp = 0;
	static constexpr int kStaleTimeoutMs = 3000;
	static constexpr int kReconnectTriggerMs = 5000;

	// GGA injection for VRS
	QTimer m_ggaTimer;
	QByteArray m_currentGga;

	// Reconnect with backoff
	QTimer m_reconnectTimer;
	int m_reconnectCount = 0;
	int m_currentBackoffMs = 0;
	static constexpr int kMaxBackoffMs = 15000;

	// Data rate tracking
	float m_dataRate = 0.0f;
	qint64 m_bytesInWindow = 0;
	qint64 m_windowStartTime = 0;
	static constexpr int kRateWindowMs = 5000;

	// HTTP response parsing
	bool m_headersParsed = false;
	QByteArray m_headerBuffer;
};


}  // namespace OpenOrienteering

#endif
