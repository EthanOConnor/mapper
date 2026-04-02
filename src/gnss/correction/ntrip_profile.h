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


#ifndef OPENORIENTEERING_NTRIP_PROFILE_H
#define OPENORIENTEERING_NTRIP_PROFILE_H

#include <cstdint>

#include <QDateTime>
#include <QString>

namespace OpenOrienteering {


/// NTRIP protocol version.
enum class NtripVersion : std::uint8_t
{
	Auto = 0,  ///< Try v2 first, fall back to v1
	V1   = 1,
	V2   = 2,
};


/// Stored configuration for an NTRIP caster connection.
///
/// Multiple profiles can be saved and quick-switched in the field.
/// Credentials are stored in the platform keychain (not in QSettings).
struct NtripProfile
{
	QString name;                        ///< User label, e.g., "RTK2Go - VRS_CMR"
	QString casterHost;                  ///< Hostname or IP
	quint16 casterPort = 2101;           ///< TCP port (NTRIP default 2101)
	QString mountpoint;                  ///< Mountpoint name
	QString username;
	QString password;                    ///< In memory only; persisted to keychain
	NtripVersion version = NtripVersion::Auto;  ///< Protocol version
	bool useTls = false;                 ///< TLS encryption
	bool sendGga = true;                 ///< Inject GGA for VRS services
	int ggaIntervalSec = 10;             ///< GGA injection interval
	QDateTime lastUsed;
	QDateTime lastSuccessful;
};


}  // namespace OpenOrienteering

#endif
