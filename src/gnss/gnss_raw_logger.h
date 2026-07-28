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


#ifndef OPENORIENTEERING_GNSS_RAW_LOGGER_H
#define OPENORIENTEERING_GNSS_RAW_LOGGER_H

#include <QFile>
#include <QObject>
#include <QString>


namespace OpenOrienteering {

class GnssRawLogger : public QObject
{
	Q_OBJECT
public:
	explicit GnssRawLogger(QObject* parent = nullptr);
	~GnssRawLogger() override;

	/// Start logging to files in the given directory.
	/// Creates timestamped files: gnss_rx_YYYYMMDD_HHmmss.ubx and gnss_ntrip_YYYYMMDD_HHmmss.rtcm
	bool start(const QString& directory);

	/// Stop logging and close files.
	void stop();

	bool isLogging() const { return m_logging; }

public slots:
	/// Log raw bytes received from the GNSS receiver (transport layer).
	void logReceiverData(const QByteArray& data);

	/// Log raw RTCM correction bytes from NTRIP.
	void logCorrectionData(const QByteArray& data);

private:
	QFile m_receiverFile;
	QFile m_correctionFile;
	bool m_logging = false;
};

}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_GNSS_RAW_LOGGER_H
