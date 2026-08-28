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

#include "hyfix_protocol.h"

#include <cstdlib>

#include <QLatin1String>

namespace OpenOrienteering {

namespace {

const char* const kNamePrefixes[] = { "GEOPULSE_", "LITERTK_", "BOOSTER_" };

QString linkToken(HyfixCorrectionLink link)
{
	switch (link) {
	case HyfixCorrectionLink::Bluetooth: return QStringLiteral("BT");
	case HyfixCorrectionLink::UsbC:      return QStringLiteral("USBC");
	case HyfixCorrectionLink::Wifi:      return QStringLiteral("WIFI");
	}
	return QStringLiteral("BT");
}

/// Parse a decimal integer field, returning -1 for anything unparseable.
int toInterval(const QString& field)
{
	bool ok = false;
	const auto value = field.trimmed().toInt(&ok);
	return (ok && value >= 0) ? value : -1;
}

bool assign(QString& target, const QString& value)
{
	if (target == value)
		return false;
	target = value;
	return true;
}

bool assign(int& target, int value)
{
	if (target == value)
		return false;
	target = value;
	return true;
}

}  // namespace


bool HyfixProtocol::isHyfixDeviceName(const QString& name)
{
	const auto normalized = name.trimmed().toUpper();
	for (const auto* prefix : kNamePrefixes)
	{
		if (normalized.startsWith(QLatin1String(prefix)))
			return true;
	}
	return false;
}


QString HyfixProtocol::friendlyName(const QString& name)
{
	if (!isHyfixDeviceName(name))
		return name;

	const auto trimmed = name.trimmed();
	const auto underscore = trimmed.indexOf(QLatin1Char('_'));
	const auto suffix = (underscore >= 0) ? trimmed.mid(underscore + 1) : QString{};
	if (suffix.isEmpty())
		return QStringLiteral("HYFIX GEO-PULSE");
	//: %1 is the receiver serial number
	return QStringLiteral("HYFIX GEO-PULSE (%1)").arg(suffix);
}


QByteArray HyfixProtocol::command(const QByteArray& body)
{
	QByteArray out;
	out.reserve(body.size() + 10);
	out += "+HYFIX,";
	out += body;
	out += "#\r\n";
	return out;
}


QByteArray HyfixProtocol::queryVersion()          { return command("VERSION?"); }
QByteArray HyfixProtocol::queryGnssVersion()      { return command("GNSSVERSION?"); }
QByteArray HyfixProtocol::querySerialNumber()     { return command("SN?"); }
QByteArray HyfixProtocol::queryWorkMode()         { return command("WORKMODE?"); }
QByteArray HyfixProtocol::queryMessageConfig()    { return command("GNSSMSG?"); }
QByteArray HyfixProtocol::queryNtripClientStatus(){ return command("NTRIPCLISTATUS?"); }
QByteArray HyfixProtocol::queryAntenna()          { return command("GNSSANT?"); }


QByteArray HyfixProtocol::setRoverMode(HyfixCorrectionLink link)
{
	return command("WORKMODE,ROVER,NTRIPCLI," + linkToken(link).toLatin1());
}


QByteArray HyfixProtocol::setNmeaIntervalMs(int interval_ms)
{
	return command("GNSSMSG,NMEA," + QByteArray::number(interval_ms));
}


QByteArray HyfixProtocol::setNavigationMode(NavigationMode mode)
{
	return transparentGnssCommand("PAIR080," + QByteArray::number(int(mode)));
}


QByteArray HyfixProtocol::enableEstimatedPositionError()
{
	return transparentGnssCommand("PQTMCFGMSGRATE,W,PQTMEPE,1,2");
}


QByteArray HyfixProtocol::transparentGnssCommand(const QByteArray& body)
{
	return command("TRANS,GNSS,$" + body + '*' + nmeaChecksum(body));
}


QVector<int> HyfixProtocol::supportedNmeaIntervalsMs()
{
	// GPv2 3.8.2 maps these onto the module's $PAIR050 fix interval and
	// rejects every other value. Verified on the bench, see the class comment.
	return { 100, 200, 500, 1000 };
}


int HyfixProtocol::nearestSupportedNmeaIntervalMs(int desired_ms)
{
	const auto supported = supportedNmeaIntervalsMs();
	int best = supported.first();
	int best_distance = std::abs(desired_ms - best);
	for (int candidate : supported)
	{
		const int distance = std::abs(desired_ms - candidate);
		if (distance < best_distance)
		{
			best = candidate;
			best_distance = distance;
		}
	}
	return best;
}


QByteArray HyfixProtocol::nmeaChecksum(const QByteArray& body)
{
	unsigned char checksum = 0;
	for (char c : body)
		checksum ^= static_cast<unsigned char>(c);

	QByteArray hex = QByteArray::number(checksum, 16).toUpper();
	if (hex.size() < 2)
		hex.prepend('0');
	return hex;
}


bool HyfixProtocol::isReplyLine(const QByteArray& line)
{
	return line.trimmed().startsWith("+HYFIX,");
}


bool HyfixProtocol::parseReply(const QByteArray& line, HyfixReply& reply)
{
	auto trimmed = line.trimmed();
	if (!trimmed.startsWith("+HYFIX,"))
		return false;

	trimmed.remove(0, int(sizeof("+HYFIX,") - 1));
	if (trimmed.endsWith('#'))
		trimmed.chop(1);
	if (trimmed.isEmpty())
		return false;

	const auto parts = trimmed.split(',');
	reply.verb = QString::fromUtf8(parts.first()).trimmed().toUpper();
	if (reply.verb.isEmpty())
		return false;

	reply.fields.clear();
	reply.fields.reserve(parts.size() - 1);
	for (int i = 1; i < parts.size(); ++i)
		reply.fields.append(QString::fromUtf8(parts.at(i)).trimmed());
	return true;
}


bool HyfixProtocol::applyReply(const HyfixReply& reply, HyfixDeviceInfo& info)
{
	auto field = [&reply](int index) {
		return index < reply.fields.size() ? reply.fields.at(index) : QString{};
	};

	bool changed = !info.identified;
	info.identified = true;

	if (reply.verb == QLatin1String("VERSION"))
	{
		// +HYFIX,VERSION,GPv2-3.8.2@20260415,3.8.2,GPv2,v2.0#
		changed |= assign(info.productBanner, field(0));
		changed |= assign(info.productFirmware, field(1));
		changed |= assign(info.hardwareModel, field(2));
		changed |= assign(info.protocolVersion, field(3));
	}
	else if (reply.verb == QLatin1String("GNSSVERSION"))
	{
		// +HYFIX,GNSSVERSION,11.04,R11A04S_CSA2#
		changed |= assign(info.gnssFirmware, field(0));
		changed |= assign(info.gnssFirmwareBuild, field(1));
	}
	else if (reply.verb == QLatin1String("SN"))
	{
		// +HYFIX,SN,<serial>,<binding ciphertext>#
		// Only the serial is kept: the second field is an account-binding
		// token and has no place in session state or diagnostics dumps.
		changed |= assign(info.serialNumber, field(0));
	}
	else if (reply.verb == QLatin1String("WORKMODE"))
	{
		// +HYFIX,WORKMODE,ROVER,NTRIPCLI,USBC#
		changed |= assign(info.workMode, field(0));
		changed |= assign(info.correctionMode, field(1));
		changed |= assign(info.correctionLink, field(2));
	}
	else if (reply.verb == QLatin1String("GNSSMSG"))
	{
		// +HYFIX,GNSSMSG,<stream>,<value># or +HYFIX,GNSSMSG,ERR,<detail>#
		const auto stream = field(0).toUpper();
		if (stream == QLatin1String("ERR"))
			changed |= assign(info.lastError, QStringLiteral("GNSSMSG ") + field(1));
		else if (stream == QLatin1String("NMEA"))
			changed |= assign(info.nmeaIntervalMs, toInterval(field(1)));
		else if (stream == QLatin1String("RTCM"))
			changed |= assign(info.rtcmIntervalMs, toInterval(field(1)));
		else if (stream == QLatin1String("DR"))
			changed |= assign(info.drIntervalMs, toInterval(field(1)));
		else if (stream == QLatin1String("IMU"))
			changed |= assign(info.imuRateHz, toInterval(field(1)));
	}
	else if (reply.verb == QLatin1String("NTRIPCLISTATUS"))
	{
		changed |= assign(info.ntripClientState, field(0));
	}
	else if (reply.verb == QLatin1String("WIFISTATUS"))
	{
		// +HYFIX,WIFISTATUS,<rssi>,<ssid>,<state>,#
		changed |= assign(info.wifiState, field(2));
	}
	else if (reply.verb == QLatin1String("GNSSANT"))
	{
		changed |= assign(info.antennaGear, toInterval(field(0)));
	}
	else if (reply.verb == QLatin1String("ERR"))
	{
		changed |= assign(info.lastError, reply.fields.join(QLatin1Char(',')));
	}

	return changed;
}


}  // namespace OpenOrienteering
