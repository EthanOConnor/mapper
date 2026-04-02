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

#include "gnss_fusion_engine.h"

#include <algorithm>
#include <cmath>

namespace OpenOrienteering {

namespace {

constexpr qint64 kPrimaryPositionFreshMs = 3500;
constexpr qint64 kAuxiliaryFreshMs = 5000;
constexpr qint64 kNmeaPairWindowMs = 4000;

QString mergeLimitation(const QString& left, const QString& right)
{
	if (left.isEmpty())
		return right;
	if (right.isEmpty() || left == right)
		return left;
	return left + QStringLiteral(" | ") + right;
}

}  // namespace


QString gnssObservationSourceName(GnssObservationSource source)
{
	switch (source) {
	case GnssObservationSource::Unknown: return QStringLiteral("Unknown");
	case GnssObservationSource::UbxNavPvt: return QStringLiteral("UBX NAV-PVT");
	case GnssObservationSource::UbxNavDop: return QStringLiteral("UBX NAV-DOP");
	case GnssObservationSource::UbxNavSat: return QStringLiteral("UBX NAV-SAT");
	case GnssObservationSource::UbxNavCov: return QStringLiteral("UBX NAV-COV");
	case GnssObservationSource::UbxNavStatus: return QStringLiteral("UBX NAV-STATUS");
	case GnssObservationSource::UbxMonVer: return QStringLiteral("UBX MON-VER");
	case GnssObservationSource::NmeaGga: return QStringLiteral("NMEA GGA");
	case GnssObservationSource::NmeaRmc: return QStringLiteral("NMEA RMC");
	case GnssObservationSource::NmeaGsa: return QStringLiteral("NMEA GSA");
	case GnssObservationSource::NmeaGsv: return QStringLiteral("NMEA GSV");
	case GnssObservationSource::NmeaGgaRmcComposite: return QStringLiteral("NMEA GGA+RMC");
	case GnssObservationSource::OsLocation: return QStringLiteral("OS Location");
	case GnssObservationSource::StructuredHub: return QStringLiteral("Structured Hub");
	case GnssObservationSource::Replay: return QStringLiteral("Replay");
	}
	return QStringLiteral("Unknown");
}


void GnssFusionEngine::reset()
{
	*this = {};
}


void GnssFusionEngine::ingest(const GnssPositionObservation& observation)
{
	switch (observation.meta.source) {
	case GnssObservationSource::UbxNavPvt:
		m_ubxPosition = {true, observation};
		break;
	case GnssObservationSource::NmeaGga:
		m_nmeaGga = {true, observation};
		break;
	case GnssObservationSource::NmeaRmc:
		m_nmeaRmc = {true, observation};
		break;
	default:
		break;
	}

	recompute();
}


void GnssFusionEngine::ingest(const GnssDopObservation& observation)
{
	switch (observation.meta.source) {
	case GnssObservationSource::UbxNavDop:
		m_ubxDop = {true, observation};
		break;
	case GnssObservationSource::NmeaGsa:
		m_nmeaDop = {true, observation};
		break;
	default:
		break;
	}

	recompute();
}


void GnssFusionEngine::ingest(const GnssSatelliteObservation& observation)
{
	switch (observation.meta.source) {
	case GnssObservationSource::UbxNavSat:
		m_ubxSatellites = {true, observation};
		break;
	case GnssObservationSource::NmeaGsv:
		m_nmeaSatellites = {true, observation};
		break;
	default:
		break;
	}

	recompute();
}


void GnssFusionEngine::ingest(const GnssCovarianceObservation& observation)
{
	if (observation.meta.source == GnssObservationSource::UbxNavCov)
		m_ubxCovariance = {true, observation};

	recompute();
}


void GnssFusionEngine::ingest(const GnssStatusObservation& observation)
{
	if (observation.meta.source == GnssObservationSource::UbxNavStatus)
		m_ubxStatus = {true, observation};

	recompute();
}


bool GnssFusionEngine::isFresh(const QDateTime& observedAt, qint64 maxAgeMs, const QDateTime& now)
{
	return observedAt.isValid() && observedAt.msecsTo(now) <= maxAgeMs;
}


GnssFieldSource GnssFusionEngine::makeFieldSource(const GnssObservationMetadata& meta, const QDateTime& now)
{
	GnssFieldSource field;
	field.source = meta.source;
	field.observedAt = meta.observedAt;
	field.nominalResolutionM = meta.horizontalResolutionM;
	field.limitation = meta.limitation;
	field.available = true;
	field.derived = meta.accuracyDerived;
	field.timestampComplete = meta.timestampHasDate && meta.timestampHasTime;
	if (meta.observedAt.isValid())
		field.ageSeconds = meta.observedAt.msecsTo(now) / 1000.0f;
	return field;
}


GnssPositionObservation GnssFusionEngine::buildCompositeNmeaPosition(const QDateTime& now, bool* available) const
{
	GnssPositionObservation composite;
	*available = false;

	bool ggaFresh = m_nmeaGga.available
	    && isFresh(m_nmeaGga.value.meta.observedAt, kPrimaryPositionFreshMs, now);
	bool rmcFresh = m_nmeaRmc.available
	    && isFresh(m_nmeaRmc.value.meta.observedAt, kPrimaryPositionFreshMs, now);

	if (ggaFresh && rmcFresh)
	{
		qint64 pairAge = std::abs(m_nmeaGga.value.meta.observedAt.msecsTo(m_nmeaRmc.value.meta.observedAt));
		if (pairAge <= kNmeaPairWindowMs)
		{
			composite = m_nmeaGga.value;
			composite.meta.source = GnssObservationSource::NmeaGgaRmcComposite;
			composite.meta.observedAt = std::max(m_nmeaGga.value.meta.observedAt, m_nmeaRmc.value.meta.observedAt);
			composite.meta.timestampHasDate = m_nmeaRmc.value.meta.timestampHasDate;
			composite.meta.timestampHasTime = m_nmeaGga.value.meta.timestampHasTime || m_nmeaRmc.value.meta.timestampHasTime;
			composite.meta.horizontalResolutionM = std::max(
			    m_nmeaGga.value.meta.horizontalResolutionM,
			    m_nmeaRmc.value.meta.horizontalResolutionM);
			composite.meta.limitation = mergeLimitation(
			    m_nmeaGga.value.meta.limitation,
			    m_nmeaRmc.value.meta.limitation);

			if (m_nmeaRmc.value.position.timestamp.isValid())
				composite.position.timestamp = m_nmeaRmc.value.position.timestamp;
			if (!std::isnan(m_nmeaRmc.value.position.groundSpeed))
				composite.position.groundSpeed = m_nmeaRmc.value.position.groundSpeed;
			if (!std::isnan(m_nmeaRmc.value.position.headingMotion))
				composite.position.headingMotion = m_nmeaRmc.value.position.headingMotion;

			*available = composite.position.valid;
			return composite;
		}
	}

	if (ggaFresh)
	{
		composite = m_nmeaGga.value;
		*available = composite.position.valid;
		return composite;
	}

	if (rmcFresh)
	{
		composite = m_nmeaRmc.value;
		*available = composite.position.valid;
		return composite;
	}

	return composite;
}


void GnssFusionEngine::applyPrimaryPosition(const GnssPositionObservation& observation, const QDateTime& now)
{
	m_solution.position = observation.position;
	m_solution.positionSource = makeFieldSource(observation.meta, now);
	m_solution.hasFreshPosition = observation.position.valid;

	if (!std::isnan(observation.position.hAccuracyP95) || !std::isnan(observation.position.hAccuracy))
		m_solution.accuracySource = makeFieldSource(observation.meta, now);
	if (!std::isnan(observation.position.groundSpeed) || !std::isnan(observation.position.headingMotion))
		m_solution.velocitySource = makeFieldSource(observation.meta, now);
	if (observation.position.timestamp.isValid())
		m_solution.timingSource = makeFieldSource(observation.meta, now);

	m_solution.summaryLimitation = observation.meta.limitation;
}


void GnssFusionEngine::applyDop(const GnssDopObservation& observation, const QDateTime& now)
{
	m_solution.position.gDOP = observation.gDOP;
	m_solution.position.pDOP = observation.pDOP;
	m_solution.position.tDOP = observation.tDOP;
	m_solution.position.vDOP = observation.vDOP;
	m_solution.position.hDOP = observation.hDOP;
	m_solution.position.nDOP = observation.nDOP;
	m_solution.position.eDOP = observation.eDOP;
	m_solution.dopSource = makeFieldSource(observation.meta, now);
	m_solution.summaryLimitation = mergeLimitation(m_solution.summaryLimitation, observation.meta.limitation);
}


void GnssFusionEngine::applySatellites(const GnssSatelliteObservation& observation, const QDateTime& now)
{
	if (observation.satellitesUsed >= 0)
		m_solution.position.satellitesUsed = static_cast<std::uint8_t>(observation.satellitesUsed);
	if (observation.satellitesVisible >= 0)
		m_solution.position.satellitesVisible = static_cast<std::uint8_t>(observation.satellitesVisible);
	m_solution.satelliteSource = makeFieldSource(observation.meta, now);
	m_solution.summaryLimitation = mergeLimitation(m_solution.summaryLimitation, observation.meta.limitation);
}


void GnssFusionEngine::applyCovariance(const GnssCovarianceObservation& observation, const QDateTime& now)
{
	m_solution.position.computeP95Ellipse(observation.covNN, observation.covNE, observation.covEE);
	m_solution.accuracySource = makeFieldSource(observation.meta, now);
	m_solution.summaryLimitation = mergeLimitation(m_solution.summaryLimitation, observation.meta.limitation);
}


void GnssFusionEngine::applyStatus(const GnssStatusObservation& observation, const QDateTime& now)
{
	m_solution.fixOK = observation.fixOK;
	m_solution.differentialSolution = observation.diffSoln;
	m_solution.carrierSolution = observation.carrSoln;
	m_solution.spoofDetection = observation.spoofDet;
	m_solution.statusSource = makeFieldSource(observation.meta, now);
	m_solution.summaryLimitation = mergeLimitation(m_solution.summaryLimitation, observation.meta.limitation);
}


void GnssFusionEngine::recompute()
{
	m_solution = {};

	const auto now = QDateTime::currentDateTimeUtc();

	if (m_ubxPosition.available
	    && isFresh(m_ubxPosition.value.meta.observedAt, kPrimaryPositionFreshMs, now))
	{
		applyPrimaryPosition(m_ubxPosition.value, now);
	}
	else
	{
		bool compositeAvailable = false;
		auto composite = buildCompositeNmeaPosition(now, &compositeAvailable);
		if (compositeAvailable)
			applyPrimaryPosition(composite, now);
	}

	if (!m_solution.hasFreshPosition)
		return;

	if (m_ubxDop.available && isFresh(m_ubxDop.value.meta.observedAt, kAuxiliaryFreshMs, now))
	{
		applyDop(m_ubxDop.value, now);
	}
	else if (m_nmeaDop.available && isFresh(m_nmeaDop.value.meta.observedAt, kAuxiliaryFreshMs, now))
	{
		applyDop(m_nmeaDop.value, now);
	}

	if (m_ubxSatellites.available
	    && isFresh(m_ubxSatellites.value.meta.observedAt, kAuxiliaryFreshMs, now))
	{
		applySatellites(m_ubxSatellites.value, now);
	}
	else if (m_nmeaSatellites.available
	         && isFresh(m_nmeaSatellites.value.meta.observedAt, kAuxiliaryFreshMs, now))
	{
		applySatellites(m_nmeaSatellites.value, now);
	}

	if (m_ubxCovariance.available
	    && isFresh(m_ubxCovariance.value.meta.observedAt, kAuxiliaryFreshMs, now)
	    && m_solution.positionSource.source == GnssObservationSource::UbxNavPvt)
	{
		applyCovariance(m_ubxCovariance.value, now);
	}

	if (m_ubxStatus.available && isFresh(m_ubxStatus.value.meta.observedAt, kAuxiliaryFreshMs, now))
		applyStatus(m_ubxStatus.value, now);
}


}  // namespace OpenOrienteering
