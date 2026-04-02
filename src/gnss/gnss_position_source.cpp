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


#include "gnss_position_source.h"

#include "gnss_session.h"


namespace OpenOrienteering {

GnssPositionSource::GnssPositionSource(QObject* parent)
 : QObject(parent)
{
	m_positionTimeoutTimer.setSingleShot(true);
	connect(&m_positionTimeoutTimer, &QTimer::timeout,
	        this, &GnssPositionSource::onPositionTimeout);
}

GnssPositionSource::~GnssPositionSource() = default;


void GnssPositionSource::setSession(GnssSession* session)
{
	if (m_session == session)
		return;

	if (m_session)
	{
		disconnect(m_session, nullptr, this, nullptr);
		m_positionTimeoutTimer.stop();
		m_hasValidPosition = false;
	}

	m_session = session;

	if (m_session)
	{
		connect(m_session, &GnssSession::positionUpdated,
		        this, &GnssPositionSource::onSessionPositionUpdated);
		connect(m_session, &GnssSession::stateChanged,
		        this, &GnssPositionSource::onSessionStateChanged);
	}
}


const GnssState& GnssPositionSource::currentState() const
{
	if (m_session)
		return m_session->currentState();

	static const GnssState empty_state;
	return empty_state;
}


void GnssPositionSource::onSessionPositionUpdated(const GnssPosition& position)
{
	m_lastPosition = position;
	m_hasValidPosition = position.valid;

	// Reset the timeout timer on each valid position
	if (position.valid)
		m_positionTimeoutTimer.start(kPositionTimeoutMs);

	emit positionUpdated(position);
}


void GnssPositionSource::onSessionStateChanged(const GnssState& state)
{
	emit stateChanged(state);
}


void GnssPositionSource::onPositionTimeout()
{
	emit positionLost();
}


}  // namespace OpenOrienteering
