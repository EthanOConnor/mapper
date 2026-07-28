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

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <Qt>
#include <QUrl>

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
	bool sendEmptyBasicAuth = false;     ///< Send "Authorization: Basic Og==" for no-auth casters that require it
	int ggaIntervalSec = 10;             ///< GGA injection interval
	QDateTime lastUsed;
	QDateTime lastSuccessful;
};


inline bool ntripProfileParsePort(const QString& text, quint16& port)
{
	bool ok = false;
	auto value = text.trimmed().toInt(&ok);
	if (!ok || value <= 0 || value > 65535)
		return false;

	port = static_cast<quint16>(value);
	return true;
}


inline QString ntripProfileCleanMountpoint(QString mountpoint)
{
	mountpoint = mountpoint.trimmed();
	while (mountpoint.startsWith(QLatin1Char('/')))
		mountpoint.remove(0, 1);
	return mountpoint;
}


inline void ntripProfileApplyHostPort(QString endpoint, NtripProfile& profile)
{
	endpoint = endpoint.trimmed();
	if (endpoint.startsWith(QLatin1String("//")))
		endpoint.remove(0, 2);

	if (endpoint.startsWith(QLatin1Char('[')))
	{
		auto const closing = endpoint.indexOf(QLatin1Char(']'));
		if (closing > 1)
		{
			profile.casterHost = endpoint.mid(1, closing - 1).trimmed();
			auto const suffix = endpoint.mid(closing + 1).trimmed();
			if (suffix.startsWith(QLatin1Char(':')))
				ntripProfileParsePort(suffix.mid(1), profile.casterPort);
			return;
		}
	}

	auto const colon = endpoint.lastIndexOf(QLatin1Char(':'));
	if (colon > 0 && endpoint.indexOf(QLatin1Char(':')) == colon)
	{
		profile.casterHost = endpoint.left(colon).trimmed();
		ntripProfileParsePort(endpoint.mid(colon + 1), profile.casterPort);
		return;
	}

	profile.casterHost = endpoint;
}


inline NtripProfile ntripProfileNormalized(NtripProfile profile)
{
	profile.name = profile.name.trimmed();
	profile.casterHost = profile.casterHost.trimmed();
	profile.mountpoint = ntripProfileCleanMountpoint(profile.mountpoint);

	auto endpoint = profile.casterHost;
	if (endpoint.isEmpty())
		return profile;

	auto const lower = endpoint.toLower();
	if (lower.startsWith(QLatin1String("http://"))
	    || lower.startsWith(QLatin1String("https://"))
	    || lower.startsWith(QLatin1String("ntrip://")))
	{
		QUrl url(endpoint);
		if (!url.host().isEmpty())
		{
			profile.casterHost = url.host().trimmed();
			if (auto const port = url.port(-1); port > 0)
				profile.casterPort = static_cast<quint16>(port);
			if (profile.mountpoint.isEmpty())
				profile.mountpoint = ntripProfileCleanMountpoint(url.path());
			if (profile.username.isEmpty())
				profile.username = url.userName();
			if (profile.password.isEmpty())
				profile.password = url.password();
			if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0)
				profile.useTls = true;
			return profile;
		}
	}

	auto const slash = endpoint.indexOf(QLatin1Char('/'));
	if (slash >= 0)
	{
		if (profile.mountpoint.isEmpty())
			profile.mountpoint = ntripProfileCleanMountpoint(endpoint.mid(slash + 1));
		endpoint = endpoint.left(slash);
	}

	ntripProfileApplyHostPort(endpoint, profile);
	return profile;
}


inline bool ntripProfileRequiresEmptyBasicAuthorization(const NtripProfile& profile)
{
	auto const normalized = ntripProfileNormalized(profile);
	return normalized.username.isEmpty()
	    && normalized.password.isEmpty()
	    && normalized.casterPort == 11634
	    && normalized.casterHost.compare(QStringLiteral("rtkdata.online"), Qt::CaseInsensitive) == 0
	    && normalized.mountpoint.compare(QStringLiteral("TFH_ITRF2020"), Qt::CaseInsensitive) == 0;
}


inline bool ntripProfileShouldSendAuthorization(const NtripProfile& profile)
{
	return !profile.username.isEmpty()
	    || !profile.password.isEmpty()
	    || profile.sendEmptyBasicAuth
	    || ntripProfileRequiresEmptyBasicAuthorization(profile);
}


inline QByteArray ntripProfileBasicAuthorizationValue(const NtripProfile& profile)
{
	QByteArray credentials = profile.username.toLatin1();
	credentials.append(':');
	credentials.append(profile.password.toLatin1());
	return credentials.toBase64();
}


}  // namespace OpenOrienteering

#endif
