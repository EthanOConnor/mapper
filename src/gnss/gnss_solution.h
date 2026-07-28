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


#ifndef OPENORIENTEERING_GNSS_SOLUTION_H
#define OPENORIENTEERING_GNSS_SOLUTION_H

#include <QDateTime>
#include <QMetaType>
#include <QString>

#include "gnss_observation.h"

namespace OpenOrienteering {


/// Attribution and limitation metadata for one fused semantic field group.
struct GnssFieldSource
{
	GnssObservationSource source = GnssObservationSource::Unknown;
	QDateTime observedAt;
	float ageSeconds = NAN;
	float nominalResolutionM = NAN;
	QString limitation;
	bool available = false;
	bool derived = false;
	bool timestampComplete = false;
};


/// Current best fused view of GNSS state for Mapper.
///
/// `position` is still the compact UI/map-facing projection, but its source and
/// limitations live alongside it so we can distinguish precise receiver facts
/// from inferred / coarsely encoded facts.
struct GnssSolutionSnapshot
{
	GnssPosition position;

	GnssFieldSource positionSource;
	GnssFieldSource accuracySource;
	GnssFieldSource velocitySource;
	GnssFieldSource timingSource;
	GnssFieldSource dopSource;
	GnssFieldSource satelliteSource;
	GnssFieldSource statusSource;

	bool hasFreshPosition = false;
	bool fixOK = false;
	bool differentialSolution = false;
	int carrierSolution = 0;
	int spoofDetection = 0;

	QString summaryLimitation;
};


}  // namespace OpenOrienteering

Q_DECLARE_METATYPE(OpenOrienteering::GnssFieldSource)
Q_DECLARE_METATYPE(OpenOrienteering::GnssSolutionSnapshot)

#endif
