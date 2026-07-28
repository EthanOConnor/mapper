/*
 *    Copyright 2025 Matthias Kühlewein
 *    Copyright 2025 Kai Pastor
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

#include <QtTest>
#include <QBuffer>
#include <QObject>

#include "core/map.h"
#include "core/map_coord.h"
#include "core/objects/object.h"
#include "core/objects/text_object.h"
#include "core/symbols/area_symbol.h"
#include "core/symbols/line_symbol.h"
#include "core/symbols/point_symbol.h"
#include "core/symbols/symbol.h"
#include "core/symbols/text_symbol.h"
#include "util/util.h"

using namespace OpenOrienteering;

Q_DECLARE_METATYPE(MapCoordVector)

/**
 * @test Tests object functions.
 */
class ObjectTest : public QObject
{
	Q_OBJECT
	
private slots:
	void objectLengthTest_data()
	{
		QTest::addColumn<int>("map_scale");
		QTest::addColumn<MapCoordVector>("coords");
		QTest::addColumn<float>("paper_length");
		QTest::addColumn<float>("paper_area");
		
		auto coords = MapCoordVector{
		    MapCoord{0.0, 0.0}, MapCoord{0.0, 5.0}, MapCoord{10.0, 5.0}, MapCoord{10.0, 0.0}, MapCoord{0.0, 0.0, MapCoord::ClosePoint}
		};
		
		QTest::newRow("Line object 1 at 1:10000") << 10000 << coords << 30.0f << 50.0f;
		QTest::newRow("Line object 1 at 1:15000") << 15000 << coords << 30.0f << 50.0f;
	}
	
	void objectLengthTest()
	{
		QFETCH(int, map_scale);
		QFETCH(MapCoordVector, coords);
		QFETCH(float, paper_length);
		QFETCH(float, paper_area);
		
		Map map;
		map.setScaleDenominator(map_scale);
		
		PathObject obj;
		QCOMPARE(obj.getPaperLength(), 0.0f);
		QCOMPARE(obj.calculatePaperArea(), 0.0f);
		QVERIFY(obj.isAreaTooSmall() == false);
		QVERIFY(obj.isLineTooShort() == false);
		
		auto line_symbol = new LineSymbol();
		map.addSymbol(line_symbol, 0);
		PathObject rectangle{line_symbol, coords, &map};
		rectangle.updatePathCoords();
		
		QCOMPARE(rectangle.getPaperLength(), paper_length);
		QCOMPARE(rectangle.getRealLength(), paper_length*map_scale/1000.0f);
		
		line_symbol->setMinimumLength(paper_length*1000 - 1);
		QVERIFY(rectangle.isLineTooShort() == false);
		
		line_symbol->setMinimumLength(paper_length*1000 + 1);
		QVERIFY(rectangle.isLineTooShort() == true);
		
		QCOMPARE(rectangle.calculatePaperArea(), paper_area);
		QCOMPARE(rectangle.calculateRealArea(), paper_area*(map_scale/1000.0f)*(map_scale/1000.0f));
	}
	
	
	void objectAreaTest_data()
	{
		QTest::addColumn<int>("map_scale");
		QTest::addColumn<MapCoordVector>("coords");
		QTest::addColumn<float>("paper_length");
		QTest::addColumn<float>("paper_area");
		
		auto coords1 = MapCoordVector{
		    MapCoord{0.0, 0.0}, MapCoord{0.0, 5.0}, MapCoord{10.0, 5.0}, MapCoord{10.0, 0.0}, MapCoord{0.0, 0.0, MapCoord::ClosePoint}
		};
		
		QTest::newRow("Area object 1 at 1:10000") << 10000 << coords1 << 30.0f << 50.0f;
		QTest::newRow("Area object 1 at 1:15000") << 15000 << coords1 << 30.0f << 50.0f;
		
		auto coords2 = MapCoordVector{
		    MapCoord{0.0, 0.0}, MapCoord{0.0, 5.0}, MapCoord{10.0, 5.0}, MapCoord{10.0, 0.0}, MapCoord{0.0, 0.0, MapCoord::ClosePoint|MapCoord::HolePoint},
		    MapCoord{5.0, 2.0}, MapCoord{5.0, 4.0}, MapCoord{8.0, 4.0}, MapCoord{8.0, 2.0}, MapCoord{5.0, 2.0, MapCoord::ClosePoint}
		};
		
		QTest::newRow("Area object 2 at 1:10000") << 10000 << coords2 << 30.0f << 44.0f;
	}
	
	void objectAreaTest()
	{
		QFETCH(int, map_scale);
		QFETCH(MapCoordVector, coords);
		QFETCH(float, paper_length);
		QFETCH(float, paper_area);
		
		Map map;
		map.setScaleDenominator(map_scale);
		
		PathObject obj;
		QCOMPARE(obj.getPaperLength(), 0.0f);
		QCOMPARE(obj.calculatePaperArea(), 0.0f);
		QVERIFY(obj.isAreaTooSmall() == false);
		QVERIFY(obj.isLineTooShort() == false);
		
		auto area_symbol = new AreaSymbol();
		map.addSymbol(area_symbol, 0);
		PathObject rectangle_area{area_symbol, coords, &map};
		rectangle_area.updatePathCoords();
		
		QCOMPARE(rectangle_area.getPaperLength(), paper_length);
		QCOMPARE(rectangle_area.getRealLength(), paper_length*map_scale/1000.0f);
		
		QCOMPARE(rectangle_area.calculatePaperArea(), paper_area);
		QCOMPARE(rectangle_area.calculateRealArea(), paper_area*(map_scale/1000.0f)*(map_scale/1000.0f));
		
		area_symbol->setMinimumArea(paper_area*1000 - 1);
		QVERIFY(rectangle_area.isAreaTooSmall() == false);
		
		area_symbol->setMinimumArea(paper_area*1000 + 1);
		QVERIFY(rectangle_area.isAreaTooSmall() == true);
	}
	
	void calcTextCoordTransformTest()
	{
		struct RotatableTextSymbol : public TextSymbol {
			RotatableTextSymbol() : TextSymbol() { setRotatable (true); }
		} text_symbol;
		QVERIFY(text_symbol.isRotatable());
		QVERIFY(text_symbol.calculateInternalScaling() > 0);
		
		TextObject object(&text_symbol);
		object.setText(QLatin1String("ABC"));
		object.setRotation(1.5);
		
		// The anchor on the map is 0,0 on the text, regardless of rotation.
		auto const anchor_map = QPointF{10.0, 10.0};
		auto const anchor_text = QPointF{0.0, 0.0};
		object.setAnchorPosition(MapCoordF(anchor_map));
		
		auto to_text = object.calcMapToTextTransform();
		QVERIFY(!to_text.isIdentity());
		
		auto to_map = object.calcTextToMapTransform();
		QVERIFY(!to_map.isIdentity());
		
		QCOMPARE(to_text.map(anchor_map), anchor_text);
		QCOMPARE(to_map.map(anchor_text), anchor_map);
		
		// Another point in the rotated text, and subject to rotation.
		auto const anchor_text_2 = QPointF{10.0, 5.0};
		QCOMPARE(to_text.map(to_map.map(anchor_text_2)), anchor_text_2);
	}

	void persistentIdentityTest()
	{
		Map map;
		auto* symbol = new PointSymbol();
		const auto symbol_id = symbol->persistentId();
		map.addSymbol(symbol, 0);
		auto* copied_symbol = duplicate(*symbol).release();
		map.addSymbol(copied_symbol, 1);
		QVERIFY(copied_symbol->persistentId() != symbol_id);

		auto* original = new PointObject(symbol);
		const auto original_id = original->persistentId();
		QVERIFY(!original_id.isEmpty());
		map.addObject(original);

		auto* undo_copy = original->duplicate();
		QCOMPARE(undo_copy->persistentId(), original_id);
		delete undo_copy;

		auto* distinct_copy = original->duplicate();
		map.addObject(distinct_copy);
		QVERIFY(distinct_copy->persistentId() != original_id);

		const auto part_id = map.getPart(0)->persistentId();
		QVERIFY(!part_id.isEmpty());

		QBuffer saved;
		QVERIFY(map.exportToIODevice(saved));
		auto saved_bytes = saved.data();
		QVERIFY(saved_bytes.contains(original_id.toUtf8()));
		QBuffer input(&saved_bytes);
		QVERIFY(input.open(QIODevice::ReadOnly));

		Map restored;
		QVERIFY(restored.importFromIODevice(input));
		QCOMPARE(restored.getSymbol(0)->persistentId(), symbol_id);
		QCOMPARE(restored.getPart(0)->persistentId(), part_id);
		QCOMPARE(restored.getPart(0)->getObject(0)->persistentId(), original_id);
		QCOMPARE(restored.getPart(0)->getObject(1)->persistentId(),
		         distinct_copy->persistentId());
	}
	
};  // class ObjectTest

/*
 * We don't need a real GUI window.
 */
namespace {
	[[maybe_unused]] const auto qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");  // clazy:exclude=non-pod-global-static
}

QTEST_MAIN(ObjectTest)
#include "object_t.moc"  // IWYU pragma: keep
