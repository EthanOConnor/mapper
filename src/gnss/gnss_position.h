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


#ifndef OPENORIENTEERING_GNSS_POSITION_H
#define OPENORIENTEERING_GNSS_POSITION_H

#include <cmath>
#include <cstdint>

#include <QDateTime>
#include <QMetaType>

namespace OpenOrienteering {


/// GNSS fix type, ordered by increasing quality.
enum class GnssFixType : std::uint8_t
{
	NoFix    = 0,
	Fix2D    = 1,
	Fix3D    = 2,
	DGPS     = 3,
	RtkFloat = 4,
	RtkFixed = 5,
};


/// How the source characterizes its reported accuracy radius.
enum class GnssAccuracyBasis : std::uint8_t
{
	/// 68% confidence (1-sigma radial). u-blox UBX, Android, iOS.
	Sigma68 = 0,
	/// 50% confidence (Circular Error Probable).
	CEP50   = 1,
	/// Unknown; we assume 68% as the most common GNSS convention.
	Unknown = 2,
};


/// A single GNSS position fix with full quality metadata.
///
/// All accuracy/DOP fields use NAN to indicate "not available."
/// Distances are in meters, angles in degrees, times in seconds.
struct GnssPosition
{
	double latitude  = 0.0;  ///< WGS84 degrees
	double longitude = 0.0;  ///< WGS84 degrees

	double altitude         = NAN;  ///< Height above WGS84 ellipsoid (m)
	double altitudeMsl      = NAN;  ///< Height above mean sea level (m)
	double geoidSeparation  = NAN;  ///< Geoid undulation (m)

	GnssFixType fixType = GnssFixType::NoFix;

	// -- Accuracy as reported by the source --
	float hAccuracy = NAN;  ///< Horizontal accuracy radius (m), at source-reported confidence
	float vAccuracy = NAN;  ///< Vertical accuracy (m), at source-reported confidence
	GnssAccuracyBasis accuracyBasis = GnssAccuracyBasis::Unknown;

	// -- P95 accuracy (computed) --
	float hAccuracyP95 = NAN;  ///< 95th percentile horizontal radius (m)
	float vAccuracyP95 = NAN;  ///< 95th percentile vertical (m)

	// -- Error ellipse from covariance (P95, when available) --
	float ellipseSemiMajorP95 = NAN;  ///< Semi-major axis (m)
	float ellipseSemiMinorP95 = NAN;  ///< Semi-minor axis (m)
	float ellipseOrientationDeg = NAN;  ///< Rotation from north, clockwise (deg)
	bool  ellipseAvailable = false;

	// -- Dilution of precision --
	float pDOP = NAN;
	float hDOP = NAN;
	float vDOP = NAN;
	float gDOP = NAN;
	float tDOP = NAN;
	float nDOP = NAN;
	float eDOP = NAN;

	// -- Satellite info --
	std::uint8_t satellitesUsed    = 0;
	std::uint8_t satellitesVisible = 0;

	// -- Correction state --
	float    correctionAge       = NAN;  ///< Seconds since last correction applied
	uint16_t correctionStationId = 0;

	// -- Velocity --
	float groundSpeed  = NAN;  ///< m/s
	float headingMotion = NAN;  ///< Degrees from north, clockwise
	float speedAccuracy = NAN;  ///< m/s

	QDateTime timestamp;  ///< UTC time of fix
	bool valid = false;   ///< True if lat/lon are from a valid fix


	// ---- P95 computation ----

	/// Rayleigh ratio: R_95 / R_68 for a 2D circular Gaussian.
	///
	/// Derivation: radial CDF is P(r <= R) = 1 - exp(-R^2 / 2sigma^2).
	/// R_68 = sigma * sqrt(2 * ln(1/(1-0.68))) ~ 1.5096 * sigma
	/// R_95 = sigma * sqrt(2 * ln(1/(1-0.95))) ~ 2.4477 * sigma
	/// Ratio = 2.4477 / 1.5096 ~ 1.6213
	static constexpr float kP95FromSigma68 = 1.6213f;

	/// R_95 / CEP (R_50) for a 2D circular Gaussian.
	/// CEP = sigma * sqrt(2 * ln(2)) ~ 1.1774 * sigma
	/// Ratio = 2.4477 / 1.1774 ~ 2.0789
	static constexpr float kP95FromCEP50 = 2.0789f;

	/// Convert a reported accuracy radius to P95 based on its stated basis.
	static float toP95(float reported, GnssAccuracyBasis basis)
	{
		if (std::isnan(reported))
			return NAN;
		switch (basis) {
		case GnssAccuracyBasis::Sigma68:
			return reported * kP95FromSigma68;
		case GnssAccuracyBasis::CEP50:
			return reported * kP95FromCEP50;
		case GnssAccuracyBasis::Unknown:
			// Assume 68% — the most common GNSS convention.
			return reported * kP95FromSigma68;
		}
		return reported * kP95FromSigma68;
	}

	/// Compute P95 fields from the reported accuracy and basis.
	void computeP95()
	{
		hAccuracyP95 = toP95(hAccuracy, accuracyBasis);
		vAccuracyP95 = toP95(vAccuracy, accuracyBasis);
	}

	/// Compute the P95 error ellipse from a 2x2 NED position covariance matrix.
	///
	/// The covariance matrix is in NED frame (meters^2):
	///   covNN  covNE
	///   covNE  covEE
	///
	/// This computes the eigenvalues of the 2x2 matrix to get the principal
	/// axes of the error ellipse, then scales to 95% confidence using the
	/// chi-squared distribution with 2 DOF: chi2_95(2) = 5.9915.
	/// Semi-axis = sqrt(eigenvalue * 5.9915).
	void computeP95Ellipse(float covNN, float covNE, float covEE)
	{
		// chi-squared 95th percentile with 2 degrees of freedom
		constexpr float kChi2_95_2dof = 5.9915f;

		float trace = covNN + covEE;
		float det = (covNN * covEE) - (covNE * covNE);
		float discriminant = (trace * trace * 0.25f) - det;

		if (discriminant < 0.0f)
			return;  // Numerically degenerate

		float sqrtDisc = std::sqrt(discriminant);
		float lambda1 = (trace * 0.5f) + sqrtDisc;  // larger eigenvalue
		float lambda2 = (trace * 0.5f) - sqrtDisc;  // smaller eigenvalue

		if (lambda1 < 0.0f || lambda2 < 0.0f)
			return;  // Not positive semi-definite

		ellipseSemiMajorP95 = std::sqrt(lambda1 * kChi2_95_2dof);
		ellipseSemiMinorP95 = std::sqrt(lambda2 * kChi2_95_2dof);

		// Orientation: angle of the eigenvector for the larger eigenvalue.
		// atan2(covNE, lambda1 - covEE) gives the angle from East axis;
		// we want degrees from North, clockwise.
		float angleRad = std::atan2(covNE, lambda1 - covEE);
		// Convert from math angle (from East, CCW) to bearing (from North, CW)
		ellipseOrientationDeg = 90.0f - angleRad * (180.0f / static_cast<float>(M_PI));
		if (ellipseOrientationDeg < 0.0f)
			ellipseOrientationDeg += 360.0f;
		if (ellipseOrientationDeg >= 360.0f)
			ellipseOrientationDeg -= 360.0f;

		ellipseAvailable = true;
	}
};


}  // namespace OpenOrienteering

Q_DECLARE_METATYPE(OpenOrienteering::GnssPosition)

#endif
