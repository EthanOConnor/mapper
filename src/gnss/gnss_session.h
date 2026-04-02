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


#ifndef OPENORIENTEERING_GNSS_SESSION_H
#define OPENORIENTEERING_GNSS_SESSION_H

#include <memory>

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "gnss_position.h"
#include "gnss_state.h"
#include "transport/gnss_transport.h"

namespace OpenOrienteering {

class GnssRawLogger;
class NtripClient;
class UbxParser;
class NmeaParser;
class ProtocolDetector;
class RtcmFramer;


/// Orchestrates a complete GNSS receiver session.
///
/// Owns and coordinates:
///   - A transport (BLE, TCP, etc.) for communication with the receiver
///   - Protocol parsers (UBX, NMEA) for decoding incoming GNSS data
///   - A protocol detector for auto-detecting the incoming data format
///   - An RTCM framer for validating outbound correction data
///   - An optional NTRIP client for fetching RTCM corrections
///
/// Data flow:
///   Transport → Protocol Detector → UBX/NMEA Parser → GnssPosition → emit
///   NTRIP Client → RTCM Framer → Transport write → Receiver
///
/// The session maintains a GnssState that aggregates position, transport,
/// correction, and receiver metadata for UI consumption.
class GnssSession : public QObject
{
	Q_OBJECT

public:
	explicit GnssSession(QObject* parent = nullptr);
	~GnssSession() override;

	/// Set the transport to use. Takes ownership.
	void setTransport(std::unique_ptr<GnssTransport> transport);

	/// Set the NTRIP client for corrections. Takes ownership.
	void setNtripClient(std::unique_ptr<NtripClient> ntrip);

	/// Start the session: connect transport (and NTRIP if configured).
	void start();

	/// Stop the session: disconnect transport and NTRIP.
	void stop();

	/// Current session state (position + transport + corrections).
	const GnssState& currentState() const { return m_state; }

	/// Most recent position fix.
	const GnssPosition& lastPosition() const { return m_state.position; }

	/// Whether the session is active (transport connecting or connected).
	bool isActive() const;

	/// Enable or disable raw stream logging.
	void setRawLogging(bool enable, const QString& directory = {});

	/// Enable auto-reconnect on transport disconnect.
	void setAutoReconnect(bool enable);

	/// Send UBX configuration commands to the receiver.
	void configureReceiver();

	/// Handle app state transitions (foreground/background).
	void handleForegroundResume();

	/// Feed raw GNSS data into the session's protocol parsers.
	/// Used when the transport is managed externally (e.g., CoreBluetooth scanner).
	void feedData(const QByteArray& data);

	/// Start the NTRIP client if one is configured.
	void startNtrip();

signals:
	/// Emitted when a new position fix is available.
	void positionUpdated(const OpenOrienteering::GnssPosition& position);

	/// Emitted when any part of the session state changes.
	void stateChanged(const OpenOrienteering::GnssState& state);

	/// Emitted when an error occurs in any component.
	void errorOccurred(const QString& source, const QString& message);

private slots:
	// Transport callbacks
	void onTransportDataReceived(const QByteArray& data);
	void onTransportStateChanged(GnssTransport::State transportState);
	void onTransportError(const QString& message);

	// UBX parser callbacks
	void onUbxPositionUpdated(const GnssPosition& position);
	void onUbxDopUpdated(float gDOP, float pDOP, float tDOP, float vDOP,
	                     float hDOP, float nDOP, float eDOP);
	void onUbxSatelliteInfoUpdated(int totalUsed, int totalVisible);
	void onUbxCovarianceUpdated(float covNN, float covNE, float covEE);
	void onUbxStatusUpdated(bool fixOK, bool diffSoln, int carrSoln, int spoofDet);
	void onUbxVersionReceived(const QString& sw, const QString& hw,
	                          const QStringList& extensions);

	// NMEA parser callbacks
	void onNmeaPositionUpdated(const GnssPosition& position);
	void onNmeaDopUpdated(float pDOP, float hDOP, float vDOP);

	// Position merge — combines UBX and NMEA data intelligently
	void mergePosition(const GnssPosition& incoming, bool fromUbx);

	// Message statistics
	void recordMessage(const QString& name);

public:
	/// Dump the last ~20s of raw data to a file in the app's Documents dir.
	/// Returns the file path, or empty string on failure.
	QString dumpRawBuffer() const;
	int rawRingEntryCount() const { return m_rawRing.size(); }
	qint64 rawRingByteCount() const { return m_rawRingBytes; }

	// NTRIP callbacks
	void onNtripCorrectionData(const QByteArray& data);
	void onNtripStateChanged();

	// RTCM framer callbacks
	void onRtcmFrameValidated(int messageType, int payloadLength);

	// Reconnect
	void onReconnectTimer();

private:
	void setupParsers();
	void connectTransportSignals();
	void connectNtripSignals();
	void emitStateChanged();
	void scheduleReconnect();

	GnssState m_state;

	std::unique_ptr<GnssTransport> m_transport;
	std::unique_ptr<NtripClient> m_ntrip;
	std::unique_ptr<UbxParser> m_ubxParser;
	std::unique_ptr<NmeaParser> m_nmeaParser;
	std::unique_ptr<RtcmFramer> m_rtcmFramer;

	bool m_protocolDetected = false;
	GnssProtocol m_detectedProtocol = GnssProtocol::Unknown;
	QByteArray m_detectionBuffer;
	QDateTime m_lastUbxTime;  ///< Last time UBX provided a position (for source priority)

	// Raw data ring buffer for diagnostics dump
	struct RawEntry {
		qint64 timestampMs;
		QByteArray data;
		char direction;  // 'R' = received from receiver, 'C' = correction from NTRIP, 'T' = sent to receiver
	};
	QVector<RawEntry> m_rawRing;
	static constexpr int kRawRingMaxBytes = 256 * 1024;  // ~256KB, covers ~20s at typical rates
	qint64 m_rawRingBytes = 0;
	void appendRawEntry(char direction, const QByteArray& data);

	// Raw logging
	std::unique_ptr<GnssRawLogger> m_rawLogger;

	// Auto-reconnect
	bool m_autoReconnect = false;
	bool m_intentionalStop = false;
	QTimer m_reconnectTimer;
	int m_reconnectBackoffMs = 0;

	static constexpr int kMaxReconnectBackoffMs = 15000;
	static constexpr int kReconnectJitterPercent = 20;
};


}  // namespace OpenOrienteering

#endif
