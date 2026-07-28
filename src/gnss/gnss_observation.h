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


#ifndef OPENORIENTEERING_GNSS_OBSERVATION_H
#define OPENORIENTEERING_GNSS_OBSERVATION_H

#include <cstdint>

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>

#include "gnss_position.h"

namespace OpenOrienteering {


/// Canonical semantic source IDs for GNSS observations and fused fields.
///
/// This is intentionally broader than the currently implemented parsers so the
/// runtime can later ingest structured hub messages, OS location APIs, or
/// replay streams without changing the fusion boundary.
enum class GnssObservationSource : std::uint8_t
{
	Unknown               = 0,
	UbxNavPvt             = 1,
	UbxNavDop             = 2,
	UbxNavSat             = 3,
	UbxNavCov             = 4,
	UbxNavStatus          = 5,
	UbxMonVer             = 6,
	NmeaGga               = 7,
	NmeaRmc               = 8,
	NmeaGsa               = 9,
	NmeaGsv               = 10,
	NmeaGgaRmcComposite   = 11,
	OsLocation            = 12,
	StructuredHub         = 13,
	Replay                = 14,
};


QString gnssObservationSourceName(GnssObservationSource source);


/// Common metadata carried by every observation.
struct GnssObservationMetadata
{
	GnssObservationSource source = GnssObservationSource::Unknown;
	QDateTime observedAt;   ///< When this observation was decoded / received.
	QString limitation;     ///< Human-readable caveat or derivation note.
	float horizontalResolutionM = NAN;  ///< Quantization / formatting floor.
	float verticalResolutionM = NAN;
	bool timestampHasDate = false;
	bool timestampHasTime = false;
	bool accuracyDerived = false;
};


/// Atomic position / velocity / accuracy fact set from one message.
struct GnssPositionObservation
{
	GnssObservationMetadata meta;
	GnssPosition position;
};


/// DOP observation. Unavailable values remain NAN.
struct GnssDopObservation
{
	GnssObservationMetadata meta;
	float gDOP = NAN;
	float pDOP = NAN;
	float tDOP = NAN;
	float vDOP = NAN;
	float hDOP = NAN;
	float nDOP = NAN;
	float eDOP = NAN;
};


/// Satellite usage / visibility observation.
struct GnssSatelliteObservation
{
	GnssObservationMetadata meta;
	int satellitesUsed = -1;
	int satellitesVisible = -1;
};


/// Horizontal covariance in the local navigation frame.
struct GnssCovarianceObservation
{
	GnssObservationMetadata meta;
	float covNN = NAN;
	float covNE = NAN;
	float covEE = NAN;
};


/// Receiver / solution status flags.
struct GnssStatusObservation
{
	GnssObservationMetadata meta;
	bool fixOK = false;
	bool diffSoln = false;
	int carrSoln = 0;
	int spoofDet = 0;
};


/// Receiver version / model information.
struct GnssVersionObservation
{
	GnssObservationMetadata meta;
	QString swVersion;
	QString hwVersion;
	QStringList extensions;
};


}  // namespace OpenOrienteering

Q_DECLARE_METATYPE(OpenOrienteering::GnssObservationSource)
Q_DECLARE_METATYPE(OpenOrienteering::GnssObservationMetadata)
Q_DECLARE_METATYPE(OpenOrienteering::GnssPositionObservation)
Q_DECLARE_METATYPE(OpenOrienteering::GnssDopObservation)
Q_DECLARE_METATYPE(OpenOrienteering::GnssSatelliteObservation)
Q_DECLARE_METATYPE(OpenOrienteering::GnssCovarianceObservation)
Q_DECLARE_METATYPE(OpenOrienteering::GnssStatusObservation)
Q_DECLARE_METATYPE(OpenOrienteering::GnssVersionObservation)

#endif
