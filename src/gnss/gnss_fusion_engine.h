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


#ifndef OPENORIENTEERING_GNSS_FUSION_ENGINE_H
#define OPENORIENTEERING_GNSS_FUSION_ENGINE_H

#include "gnss_observation.h"
#include "gnss_solution.h"

namespace OpenOrienteering {


/// Observation-store + fusion policy for the current single active receiver.
///
/// The engine is deliberately deterministic: every update stores the latest
/// observation per semantic source and recomputes a fresh solution snapshot.
/// This gives us a stable seam for later multi-source fabrics, correction
/// semantics, or host-side solving without carrying parser-specific mutable
/// state through the UI.
class GnssFusionEngine
{
public:
	void reset();

	void ingest(const GnssPositionObservation& observation);
	void ingest(const GnssDopObservation& observation);
	void ingest(const GnssSatelliteObservation& observation);
	void ingest(const GnssCovarianceObservation& observation);
	void ingest(const GnssStatusObservation& observation);

	const GnssSolutionSnapshot& solution() const { return m_solution; }

private:
	template <typename T>
	struct Slot
	{
		bool available = false;
		T value;
	};

	void recompute();
	static bool isFresh(const QDateTime& observedAt, qint64 maxAgeMs, const QDateTime& now);
	static GnssFieldSource makeFieldSource(const GnssObservationMetadata& meta, const QDateTime& now);

	GnssPositionObservation buildCompositeNmeaPosition(const QDateTime& now, bool* available) const;
	void applyPrimaryPosition(const GnssPositionObservation& observation, const QDateTime& now);
	void applyDop(const GnssDopObservation& observation, const QDateTime& now);
	void applySatellites(const GnssSatelliteObservation& observation, const QDateTime& now);
	void applyCovariance(const GnssCovarianceObservation& observation, const QDateTime& now);
	void applyStatus(const GnssStatusObservation& observation, const QDateTime& now);

	Slot<GnssPositionObservation> m_ubxPosition;
	Slot<GnssPositionObservation> m_nmeaGga;
	Slot<GnssPositionObservation> m_nmeaRmc;
	Slot<GnssDopObservation> m_ubxDop;
	Slot<GnssDopObservation> m_nmeaDop;
	Slot<GnssSatelliteObservation> m_ubxSatellites;
	Slot<GnssSatelliteObservation> m_nmeaSatellites;
	Slot<GnssCovarianceObservation> m_ubxCovariance;
	Slot<GnssStatusObservation> m_ubxStatus;

	GnssSolutionSnapshot m_solution;
};


}  // namespace OpenOrienteering

#endif
