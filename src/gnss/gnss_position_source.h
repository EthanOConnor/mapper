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


#ifndef OPENORIENTEERING_GNSS_POSITION_SOURCE_H
#define OPENORIENTEERING_GNSS_POSITION_SOURCE_H

#include <QObject>
#include <QTimer>

#include "gnss_position.h"
#include "gnss_state.h"

namespace OpenOrienteering {

class GnssSession;
class Georeferencing;


/// Adapter that bridges GnssSession to the Mapper GPS display system.
///
/// Connects to a GnssSession's position updates, tracks position
/// timeout (loss of fix), and provides the latest position and state
/// for consumption by GPSDisplay and other UI components.
class GnssPositionSource : public QObject
{
	Q_OBJECT

public:
	explicit GnssPositionSource(QObject* parent = nullptr);
	~GnssPositionSource() override;

	/// Set the GNSS session to bridge. Connects signals.
	/// Pass nullptr to disconnect.
	void setSession(GnssSession* session);

	/// The connected session, or nullptr.
	GnssSession* session() const { return m_session; }

	/// Most recent GNSS position.
	const GnssPosition& lastPosition() const { return m_lastPosition; }

	/// Full session state (position + transport + corrections).
	const GnssState& currentState() const;

	/// Whether a valid position has been received since the session started.
	bool hasValidPosition() const { return m_hasValidPosition; }

signals:
	/// Emitted when a new GNSS position fix is available.
	void positionUpdated(const OpenOrienteering::GnssPosition& position);

	/// Emitted when position updates stop arriving (timeout).
	void positionLost();

	/// Emitted when the session state changes (transport, corrections, etc.).
	void stateChanged(const OpenOrienteering::GnssState& state);

private slots:
	void onSessionPositionUpdated(const OpenOrienteering::GnssPosition& position);
	void onSessionStateChanged(const OpenOrienteering::GnssState& state);
	void onPositionTimeout();

private:
	GnssSession* m_session = nullptr;
	GnssPosition m_lastPosition;
	bool m_hasValidPosition = false;
	QTimer m_positionTimeoutTimer;

	static constexpr int kPositionTimeoutMs = 5000;
};


}  // namespace OpenOrienteering

#endif
