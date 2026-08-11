/*
 *    Copyright 2019-2020 Kai Pastor
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

#include <memory>
#include <utility>

#include <QtTest>
#include <QDir>           // IWYU pragma: keep
#include <QFileInfo>      // IWYU pragma: keep
#include <QObject>
#include <QSignalSpy>     // IWYU pragma: keep
#include <QStandardPaths> // IWYU pragma: keep
#include <QStaticPlugin>  // IWYU pragma: keep
#include <QTemporaryDir>  // IWYU pragma: keep
#include <QTemporaryFile> // IWYU pragma: keep

#include "test_config.h"  // IWYU pragma: keep
#include "core/map.h"
#include "core/track.h"
#include "sensors/gps_track_recorder.h"
#include "templates/template_track.h"


namespace OpenOrienteering {}
using namespace OpenOrienteering;


#ifdef QT_POSITIONING_LIB
#include <QGeoPositionInfoSource>
#endif

#ifdef MAPPER_USE_FAKE_POSITION_PLUGIN
#include <QGeoCoordinate>           // IWYU pragma: keep
#include <QNmeaPositionInfoSource>  // IWYU pragma: keep
#include "sensors/fake_position_plugin.h"
#include "sensors/fake_position_source.h"
Q_IMPORT_PLUGIN(FakePositionPlugin)
#endif

#ifdef MAPPER_USE_NMEA_POSITION_PLUGIN
#include <QNmeaPositionInfoSource>  // IWYU pragma: keep
#include "sensors/nmea_position_plugin.h"
Q_IMPORT_PLUGIN(NmeaPositionPlugin)
#endif

#ifdef MAPPER_USE_POWERSHELL_POSITION_PLUGIN
#include "sensors/powershell_position_source.h"
Q_IMPORT_PLUGIN(PowershellPositionPlugin)
#endif


class SensorsTest : public QObject
{
	Q_OBJECT
	
private slots:
	void initTestCase()
	{
		QDir::addSearchPath(QStringLiteral("testdata"), QDir(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR)).absoluteFilePath(QStringLiteral("data")));
	}

	void gpsTrackRecorderPersistsAtDurabilityBoundaries()
	{
		QTemporaryDir directory(
		  QDir::current().filePath(QStringLiteral("sensors_t-XXXXXX")));
		QVERIFY(directory.isValid());

		Map map;

		const auto verify_saved_point = [](const QString& path) {
			Track saved_track;
			return saved_track.loadFrom(path, false)
			       && saved_track.getNumSegments() == 1
			       && saved_track.getSegmentPointCount(0) == 1;
		};

		const auto interrupted_path =
		  directory.filePath(QStringLiteral("interrupted.gpx"));
		TemplateTrack interrupted_track(interrupted_path, &map);
		interrupted_track.configureForGPSTrack();
		{
			GPSTrackRecorder recorder(nullptr, &interrupted_track);
			recorder.newPosition(47.6101, -122.2015, 118.0, 0.8f);
			QVERIFY(interrupted_track.hasUnsavedChanges());
			recorder.positionUpdatesInterrupted();
			QVERIFY(!interrupted_track.hasUnsavedChanges());
			QVERIFY(verify_saved_point(interrupted_path));
		}

		const auto destroyed_path =
		  directory.filePath(QStringLiteral("destroyed.gpx"));
		TemplateTrack destroyed_track(destroyed_path, &map);
		destroyed_track.configureForGPSTrack();
		{
			GPSTrackRecorder recorder(nullptr, &destroyed_track);
			recorder.newPosition(47.6102, -122.2016, 119.0, 0.7f);
			QVERIFY(destroyed_track.hasUnsavedChanges());
		}
		QVERIFY(!destroyed_track.hasUnsavedChanges());
		QVERIFY(verify_saved_point(destroyed_path));

		const auto deleted_path =
		  directory.filePath(QStringLiteral("deleted.gpx"));
		auto deleted_track = std::make_unique<TemplateTrack>(deleted_path, &map);
		deleted_track->configureForGPSTrack();
		auto* deleted_track_ptr = deleted_track.get();
		map.addTemplate(0, std::move(deleted_track));
		{
			GPSTrackRecorder recorder(nullptr, deleted_track_ptr);
			recorder.newPosition(47.6103, -122.2017, 120.0, 0.6f);
			map.deleteTemplate(0);
		}
		QVERIFY(verify_saved_point(deleted_path));
	}
	
	
#if defined(MAPPER_USE_FAKE_POSITION_PLUGIN)
	void fakePositionSourcePluginTest()
	{
		auto sources = QGeoPositionInfoSource::availableSources();
		QVERIFY(sources.contains(QStringLiteral("Fake position")));
	}
	
	void fakePositionSourceSimulatedTest()
	{
		auto const reference = QGeoCoordinate{10, 20, 100};
		FakePositionSource::setReferencePoint(reference);
		
		auto* source = QGeoPositionInfoSource::createSource(QStringLiteral("Fake position"), this);
		QVERIFY(source);
		QCOMPARE(int(source->error()), int(QGeoPositionInfoSource::NoError));
		
		QSignalSpy source_spy(source, &QGeoPositionInfoSource::positionUpdated);
		QVERIFY(source_spy.isValid());
		
		source->startUpdates();
		QCOMPARE(int(source->error()), int(QGeoPositionInfoSource::NoError));
		QVERIFY(source_spy.wait());
		
		auto last = source->lastKnownPosition(true);
		QVERIFY(last.isValid());
		QVERIFY(qAbs(last.coordinate().latitude() - reference.latitude()) < 0.002);
		QVERIFY(qAbs(last.coordinate().longitude() - reference.longitude()) < 0.002);
		QVERIFY(qAbs(last.coordinate().altitude() - reference.altitude()) < 20);
		
		source->stopUpdates();
		delete source;
	}
#endif  // MAPPER_USE_FAKE_POSITION_PLUGIN
	
	
#if defined(MAPPER_USE_NMEA_POSITION_PLUGIN)
	void nmeaPositionSourcePluginTest()
	{
		auto nmea_position_source = QStringLiteral("NMEA (OpenOrienteering)");
		auto sources = QGeoPositionInfoSource::availableSources();
		QVERIFY(sources.contains(nmea_position_source));
	}
	
	void nmeaPositionSourceSimulatedTest()
	{
		auto nmea_position_source = QStringLiteral("NMEA (OpenOrienteering)");
		auto* source = QGeoPositionInfoSource::createSource(nmea_position_source, this);
#if !defined(Q_OS_LINUX) && !defined(Q_OS_MACOS)
		QVERIFY(!source || source->error() != QGeoPositionInfoSource::NoError);
		QSKIP("\"unsupported platform");
#endif
		QVERIFY(source);
		QVERIFY(qobject_cast<QNmeaPositionInfoSource*>(source));
		QCOMPARE(int(source->error()), int(QGeoPositionInfoSource::NoError));
		
		QSignalSpy source_spy(source, &QGeoPositionInfoSource::positionUpdated);
		QVERIFY(source_spy.isValid());
		
		auto test_file = QFileInfo(QStringLiteral("testdata:sensors/nmea.txt"));
		QVERIFY(test_file.exists());
		qputenv("QT_NMEA_SERIAL_PORT", test_file.absoluteFilePath().toUtf8());
		source->startUpdates();
		QCOMPARE(int(source->error()), int(QGeoPositionInfoSource::NoError));
		QVERIFY(source_spy.wait());
		
		auto last = source->lastKnownPosition(true);
		QVERIFY(last.isValid());
		QCOMPARE(int(last.coordinate().latitude()), -30);
		QCOMPARE(int(last.coordinate().longitude()), 139);
		
		source->stopUpdates();
		
		QTemporaryFile unreadable_file;
		QVERIFY(unreadable_file.open());
		unreadable_file.close();
		unreadable_file.setPermissions({});
		qputenv("QT_NMEA_SERIAL_PORT", unreadable_file.fileName().toUtf8());
		source->startUpdates();
		QCOMPARE(int(source->error()), int(QGeoPositionInfoSource::AccessError));
		
		delete source;
	}
#endif  // MAPPER_USE_NMEA_POSITION_PLUGIN
	
#if defined(MAPPER_USE_POWERSHELL_POSITION_PLUGIN)
	void powershellPositionSourcePluginTest()
	{
		auto powershell_position_source = QStringLiteral("Windows");
		auto sources = QGeoPositionInfoSource::availableSources();
		QVERIFY(sources.contains(powershell_position_source));  // JSON file properties
	}
		
	void powershellPositionSourceLiveTest()
	{
		if(!qEnvironmentVariableIsEmpty("SKIP_POWERSHELL_POSITION_SOURCE_LIVE_TEST"))
			QSKIP("SKIP_POWERSHELL_POSITION_SOURCE_LIVE_TEST is set");
		
		auto powershell_path = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
#if !defined(Q_OS_WIN)
		if (powershell_path.isEmpty())
			QSKIP("Powershell not available");
#endif
		QVERIFY(!powershell_path.isEmpty());
		
		auto powershell_position_source = QStringLiteral("Windows");
		auto* source = QGeoPositionInfoSource::createSource(powershell_position_source, this);
		QVERIFY(source);
		QCOMPARE(source->error(), QGeoPositionInfoSource::NoError);
		
		QSignalSpy source_spy(source, &QGeoPositionInfoSource::positionUpdated);
		source->startUpdates();
		if (source_spy.wait(2000))
			return;
		
		switch(source->error())
		{
		case QGeoPositionInfoSource::NoError:
			qWarning("startUpdates(): QGeoPositionInfoSource::NoError");  // and no position yet!
			break;
		case QGeoPositionInfoSource::AccessError:
			qWarning("startUpdates(): QGeoPositionInfoSource::AccessError");
			break;
		case QGeoPositionInfoSource::UnknownSourceError:
			qWarning("startUpdates(): QGeoPositionInfoSource::UnknownSourceError");
			break;
		case QGeoPositionInfoSource::UpdateTimeoutError:
			qWarning("startUpdates(): QGeoPositionInfoSource::UpdateTimeoutError");
			break;
		case QGeoPositionInfoSource::ClosedError:
			QFAIL("startUpdates(): QGeoPositionInfoSource::ClosedError");
			break;
		default:
			QFAIL("startUpdates(): unknown error");
			break;
		}
	}
	
	static QGeoPositionInfoSource::Error powershellPositionSourceSimulationSetup(QProcess& process, QByteArray& /* unused */, QByteArray& /* unused */)
	{
		auto test_file = QFileInfo(QStringLiteral("testdata:sensors/powershell_position.txt"));
		if (!test_file.exists())
			return QGeoPositionInfoSource::UnknownSourceError;
		
#if defined(Q_OS_WIN)
		process.setProgram(QStandardPaths::findExecutable(QStringLiteral("cmd")));
		process.setArguments({QStringLiteral("/C"), QStringLiteral("type"), QDir::toNativeSeparators(test_file.absoluteFilePath())});
#else
		process.setProgram(QStringLiteral("/bin/cat"));
		process.setArguments({test_file.absoluteFilePath()});
#endif
		return QGeoPositionInfoSource::NoError;
	};
	
	void powershellPositionSourceSimulatedTest()
	{
		PowershellPositionSource source(powershellPositionSourceSimulationSetup);
		QCOMPARE(int(source.error()), int(QGeoPositionInfoSource::NoError));
		
		QSignalSpy source_spy(&source, &QGeoPositionInfoSource::positionUpdated);
		QVERIFY(source_spy.isValid());
		source.startUpdates();
		QCOMPARE(int(source.error()), int(QGeoPositionInfoSource::NoError));
		QVERIFY(source_spy.wait());
		
		auto last = source.lastKnownPosition(true);
		QVERIFY(last.isValid());
		QCOMPARE(int(last.coordinate().latitude()), 50);
		QCOMPARE(int(last.coordinate().longitude()), 8);
		
		source.stopUpdates();
	}
#endif  // MAPPER_USE_POWERSHELL_POSITION_PLUGIN
	
};


/*
 * We don't need a real GUI window.
 * 
 * But we discovered QTBUG-58768 macOS: Crash when using QPrinter
 * while running with "minimal" platform plugin.
 */
#ifndef Q_OS_MACOS
namespace  {
	[[maybe_unused]] const auto qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");  // clazy:exclude=non-pod-global-static
}
#endif


QTEST_MAIN(SensorsTest)
#include "sensors_t.moc"  // IWYU pragma: keep
