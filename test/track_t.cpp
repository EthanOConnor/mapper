/*
 *    Copyright 2018, 2024 Kai Pastor
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

#include <cmath>

#include <Qt>
#include <QtGlobal>
#include <QtTest>
#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLatin1String>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTimeZone>
#include <QXmlStreamWriter>

#include "global.h"
#include "test_config.h"
#include "core/track.h"

using namespace OpenOrienteering;


/**
 * @test Tests GPX tracks.
 */
class TrackTest : public QObject
{
	Q_OBJECT
	
	QDateTime base_datetime = QDateTime::fromMSecsSinceEpoch(0, QTimeZone{QTimeZone::UTC}).addYears(40);
	
private slots:
	void initTestCase()
	{
		// Use distinct QSettings
		QCoreApplication::setOrganizationName(QString::fromLatin1("OpenOrienteering.org"));
		QCoreApplication::setApplicationName(QString::fromLatin1(metaObject()->className()));
		QVERIFY2(QDir::home().exists(), "The home dir must be writable in order to use QSettings.");
		
		QDir::addSearchPath(QStringLiteral("testdata"), QDir(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR)).absoluteFilePath(QStringLiteral("data")));
		doStaticInitializations();
	}
	
	
	void gpxTest_data()
	{
		struct
		{
			const char* path_out;  // expected output
			const char* path_in;   // input to be preloaded
			const int offset;      // seconds to be added to new timestamps
		} const track_test_files[] = {
			{ "testdata:track/track-0.gpx", nullptr, 0 },
			{ "testdata:track/track-1.gpx", "testdata:track/track-0.gpx", 60 },
		};
		
		QTest::addColumn<QString>("filename_out");
		QTest::addColumn<QString>("filename_in");
		QTest::addColumn<int>("offset");
		for (auto test_file : track_test_files)
		{
			QTest::newRow(test_file.path_out) << QString::fromUtf8(test_file.path_out)
			                                  << QString::fromUtf8(test_file.path_in)
			                                  << test_file.offset;
		}
	}
	
	void gpxTest()
	{
		QFETCH(QString, filename_out);
		QFETCH(QString, filename_in);
		QFETCH(int, offset);
		
		auto actual_track = Track{};
		if (!filename_in.isEmpty())
			QVERIFY(actual_track.loadFrom(filename_in, true));
		
		const auto waypoint_name = filename_out.right(11);
		const auto wp0 = TrackPoint{ {50.0, 7.1}, base_datetime.addSecs(offset + 0), 105, 24};
		actual_track.appendWaypoint(wp0, waypoint_name);
		
		const auto tp0 = TrackPoint{ {50.0, 7.0}, base_datetime.addSecs(offset + 1), 100};
		actual_track.appendTrackPoint(tp0);
		const auto tp1 = TrackPoint{ {50.1, 7.0}, base_datetime.addSecs(offset + 2), 110, 28 };
		actual_track.appendTrackPoint(tp1);
		const auto tp2 = TrackPoint{ {50.1, 7.1}, base_datetime.addSecs(offset + 3), NAN, 32 };
		actual_track.appendTrackPoint(tp2);
		actual_track.finishCurrentSegment();
		
		const auto tp3 = TrackPoint{ {50.0, 7.1}, base_datetime.addSecs(offset + 9), 105 };
		actual_track.appendTrackPoint(tp3);
		actual_track.finishCurrentSegment();
		
		auto filename_tmp = QFileInfo(filename_out).fileName();
		filename_tmp.insert(filename_tmp.length() - 4, QLatin1String(".tmp"));
		QVERIFY(actual_track.saveTo(filename_tmp));
		
		auto readAll = [](QFile&& file) -> QByteArray {
			if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
				return {};
			return file.readAll();
		};
		const auto raw_tmp = readAll(QFile(filename_tmp));
		QVERIFY(!raw_tmp.isEmpty());
		const auto raw_expected = readAll(QFile(filename_out));
		QVERIFY(!raw_expected.isEmpty());
		QCOMPARE(raw_tmp, raw_expected);
		
		auto expected_track = Track{};
		QVERIFY(expected_track.loadFrom(filename_out, true));
		QCOMPARE(actual_track, expected_track);
		
		QVERIFY(QFile::remove(filename_tmp));
	}

	void gpxMillisecondTimestampTest()
	{
		QByteArray output;
		QXmlStreamWriter stream(&output);
		stream.writeStartDocument();
		stream.writeStartElement(QStringLiteral("trkpt"));
		TrackPoint point{{50.0, 7.0}, base_datetime.addMSecs(250), 100};
		point.save(&stream);
		stream.writeEndElement();
		stream.writeEndDocument();
		QVERIFY(output.contains("<time>2010-01-01T00:00:00.250Z</time>"));
	}
	
	
	void gnssQualityRoundTripTest()
	{
		auto track = Track{};

		auto rtk_point = TrackPoint{ {47.61, -122.20}, base_datetime.addSecs(1), 118.5f, 0.8f };
		rtk_point.hAccuracy = 0.014f;
		rtk_point.vAccuracy = 0.021f;
		rtk_point.correctionAge = 1.5f;
		rtk_point.fixType = TrackFixType::RtkFixed;
		rtk_point.satellitesUsed = 29;
		rtk_point.accuracyBasis = QStringLiteral("gst");
		track.appendTrackPoint(rtk_point);

		auto float_point = TrackPoint{ {47.62, -122.21}, base_datetime.addSecs(2) };
		float_point.hAccuracy = 0.12f;
		float_point.fixType = TrackFixType::RtkFloat;
		track.appendTrackPoint(float_point);

		auto plain_point = TrackPoint{ {47.63, -122.22}, base_datetime.addSecs(3), 120.0f };
		track.appendTrackPoint(plain_point);
		track.finishCurrentSegment();

		QBuffer buffer;
		QVERIFY(buffer.open(QIODevice::ReadWrite));
		QVERIFY(track.saveGpxTo(buffer));

		const auto data = buffer.data();
		QVERIFY(data.contains("xmlns=\"http://www.topografix.com/GPX/1/1\""));
		QVERIFY(data.contains("xmlns:mapper=\"http://openorienteering.org/xmlns/mapper-gnss/1\""));
		QVERIFY(data.contains("<mapper:gnss"));
		QVERIFY(data.contains("fix=\"rtk-fixed\""));
		QVERIFY(data.contains("fix=\"rtk-float\""));
		QVERIFY(data.contains("hacc=\"0.014\""));
		QVERIFY(data.contains("vacc=\"0.021\""));
		QVERIFY(data.contains("basis=\"gst\""));
		QVERIFY(data.contains("corr_age=\"1.500\""));
		QVERIFY(data.contains("<sat>29</sat>"));
		// The best standard approximation of an RTK fix:
		QVERIFY(data.contains("<fix>dgps</fix>"));
		// True DOP is written as <hdop>:
		QVERIFY(data.contains("<hdop>0.800</hdop>"));

		QVERIFY(buffer.seek(0));
		auto loaded_track = Track{};
		QVERIFY(loaded_track.loadGpxFrom(buffer, true));
		QCOMPARE(loaded_track, track);

		QCOMPARE(loaded_track.getSegmentPoint(0, 0).fixType, TrackFixType::RtkFixed);
		QCOMPARE(loaded_track.getSegmentPoint(0, 0).satellitesUsed, 29);
		QCOMPARE(loaded_track.getSegmentPoint(0, 1).fixType, TrackFixType::RtkFloat);
		QCOMPARE(loaded_track.getSegmentPoint(0, 2).fixType, TrackFixType::Unknown);
		QVERIFY(qIsNaN(loaded_track.getSegmentPoint(0, 2).hAccuracy));
	}


	void legacyGpxLoadTest()
	{
		// Legacy Mapper output: no namespaces, no fix/sat/extensions.
		// The foreign extensions subtree must be skipped completely, i.e.
		// its nested ele/time/hdop must not be misparsed as point data.
		QByteArray legacy_gpx(
		  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		  "<gpx version=\"1.1\" creator=\"OpenOrienteering Mapper\">\n"
		  "<trk>\n"
		  "<trkseg>\n"
		  "<trkpt lat=\"50.000000000000\" lon=\"7.000000000000\">"
		  "<time>2010-01-01T00:00:01Z</time><ele>100.000</ele><hdop>1.500</hdop>"
		  "<extensions><vendor:stats xmlns:vendor=\"http://example.com/x/1\">"
		  "<vendor:ele>999.0</vendor:ele><vendor:hdop>77.0</vendor:hdop>"
		  "</vendor:stats></extensions>"
		  "</trkpt>\n"
		  "</trkseg>\n"
		  "</trk>\n"
		  "</gpx>\n");
		QBuffer buffer(&legacy_gpx);
		QVERIFY(buffer.open(QIODevice::ReadOnly));

		auto track = Track{};
		QVERIFY(track.loadGpxFrom(buffer, false));
		QCOMPARE(track.getNumSegments(), 1);
		QCOMPARE(track.getSegmentPointCount(0), 1);

		const auto& point = track.getSegmentPoint(0, 0);
		QCOMPARE(point.latlon.latitude(), 50.0);
		QCOMPARE(point.elevation, 100.0f);
		QCOMPARE(point.hDOP, 1.5f);
		// Unknown quality data stays unknown:
		QVERIFY(qIsNaN(point.hAccuracy));
		QVERIFY(qIsNaN(point.vAccuracy));
		QVERIFY(qIsNaN(point.correctionAge));
		QCOMPARE(point.fixType, TrackFixType::Unknown);
		QCOMPARE(point.satellitesUsed, -1);
		QVERIFY(point.accuracyBasis.isEmpty());
	}


	void noHdopFromAccuracyTest()
	{
		// A point which only knows accuracy in meters must not emit <hdop>.
		auto track = Track{};
		auto point = TrackPoint{ {50.0, 7.0}, base_datetime };
		point.hAccuracy = 3.7f;
		track.appendTrackPoint(point);
		track.finishCurrentSegment();

		QBuffer buffer;
		QVERIFY(buffer.open(QIODevice::ReadWrite));
		QVERIFY(track.saveGpxTo(buffer));

		const auto data = buffer.data();
		QVERIFY(!data.contains("<hdop>"));
		QVERIFY(data.contains("hacc=\"3.700\""));
	}

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


QTEST_MAIN(TrackTest)
#include "track_t.moc"  // IWYU pragma: keep
