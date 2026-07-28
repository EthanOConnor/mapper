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


#include "gnss_raw_logger.h"

#include <QDateTime>
#include <QDir>
#include <QIODevice>
#include <QtLogging>


namespace OpenOrienteering {

GnssRawLogger::GnssRawLogger(QObject* parent)
 : QObject(parent)
{
}

GnssRawLogger::~GnssRawLogger()
{
	stop();
}

bool GnssRawLogger::start(const QString& directory)
{
	auto timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
	auto dir = QDir(directory);

	m_receiverFile.setFileName(dir.filePath(QStringLiteral("gnss_rx_") + timestamp + QStringLiteral(".ubx")));
	m_correctionFile.setFileName(dir.filePath(QStringLiteral("gnss_ntrip_") + timestamp + QStringLiteral(".rtcm")));

	if (!m_receiverFile.open(QIODevice::WriteOnly))
	{
		qWarning("GnssRawLogger: cannot open receiver log %s", qPrintable(m_receiverFile.fileName()));
		return false;
	}

	if (!m_correctionFile.open(QIODevice::WriteOnly))
	{
		qWarning("GnssRawLogger: cannot open correction log %s", qPrintable(m_correctionFile.fileName()));
		m_receiverFile.close();
		return false;
	}

	m_logging = true;
	return true;
}

void GnssRawLogger::stop()
{
	if (!m_logging)
		return;

	m_receiverFile.close();
	m_correctionFile.close();
	m_logging = false;
}

void GnssRawLogger::logReceiverData(const QByteArray& data)
{
	if (!m_logging)
		return;

	m_receiverFile.write(data);
	m_receiverFile.flush();
}

void GnssRawLogger::logCorrectionData(const QByteArray& data)
{
	if (!m_logging)
		return;

	m_correctionFile.write(data);
	m_correctionFile.flush();
}

}  // namespace OpenOrienteering
