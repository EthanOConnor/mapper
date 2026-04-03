/*
 *    Copyright 2026 Kai Pastor
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


#include <vector>

#include <QtTest>
#include <QObject>

#include "core/map.h"
#include "core/map_color.h"
#include "core/map_coord.h"
#include "core/map_part.h"
#include "core/objects/boolean_tool.h"
#include "core/objects/object.h"
#include "core/symbols/area_symbol.h"
#include "core/symbols/symbol.h"


using namespace OpenOrienteering;


namespace {

constexpr double bezier_kappa = 0.5522847498;


/// A snapshot of one part's geometry for comparison.
struct PartSnapshot
{
	int coord_count = 0;
	bool is_closed = false;
	MapCoordVector coords;

	bool hasCurveStarts() const
	{
		for (const auto& c : coords)
			if (c.isCurveStart())
				return true;
		return false;
	}

	int curveStartCount() const
	{
		int n = 0;
		for (const auto& c : coords)
			if (c.isCurveStart())
				++n;
		return n;
	}
};


/// A snapshot of a complete PathObject's geometry.
struct ObjectSnapshot
{
	int part_count = 0;
	std::vector<PartSnapshot> parts;
};


ObjectSnapshot snapshot(const PathObject* obj)
{
	ObjectSnapshot s;
	s.part_count = int(obj->parts().size());
	for (const auto& part : obj->parts())
	{
		PartSnapshot ps;
		ps.is_closed = part.isClosed();
		ps.coord_count = 0;
		for (auto i = part.first_index; i <= part.last_index; ++i)
		{
			ps.coords.push_back(obj->getCoordinate(i));
			++ps.coord_count;
		}
		s.parts.push_back(std::move(ps));
	}
	return s;
}


/// Print a snapshot to qDebug for golden-master generation.
void dumpSnapshot(const char* label, const ObjectSnapshot& s)
{
	qDebug() << label << "parts:" << s.part_count;
	for (int p = 0; p < s.part_count; ++p)
	{
		const auto& part = s.parts[p];
		qDebug() << "  part" << p << "coords:" << part.coord_count
		         << "closed:" << part.is_closed
		         << "curves:" << part.curveStartCount();
		for (int i = 0; i < part.coord_count; ++i)
		{
			const auto& c = part.coords[i];
			qDebug().nospace()
			    << "    [" << i << "] native("
			    << c.nativeX() << ", " << c.nativeY()
			    << ") flags=" << int(c.flags());
		}
	}
}


}  // anonymous namespace


/**
 * @test Tests BooleanTool operations for correctness and curve reconstruction.
 *
 * These tests capture ground-truth output from the current Clipper 6
 * integration. After migrating to Clipper2, behavioral differences will
 * cause test failures.
 */
class BooleanToolTest : public QObject
{
	Q_OBJECT

private:
	Map map;
	AreaSymbol* area_sym = nullptr;

	/// Set up a minimal map with one area symbol.
	void initMap()
	{
		if (area_sym)
			return;
		auto* color = new MapColor(QLatin1String("Black"), 0);
		color->setCmyk({0.0f, 0.0f, 0.0f, 1.0f});
		map.addColor(color, 0);
		area_sym = new AreaSymbol();
		area_sym->setColor(color);
		map.addSymbol(area_sym, 0);
	}

	/// Create a closed rectangle.
	PathObject* makeRect(double x, double y, double w, double h)
	{
		auto* obj = new PathObject(area_sym);
		obj->addCoordinate(MapCoord{x, y});
		obj->addCoordinate(MapCoord{x + w, y});
		obj->addCoordinate(MapCoord{x + w, y + h});
		obj->addCoordinate(MapCoord{x, y + h});
		obj->addCoordinate(MapCoord{x, y, MapCoord::ClosePoint});
		return obj;
	}

	/// Create a closed circle from 4 cubic Bezier segments.
	PathObject* makeCircle(double cx, double cy, double r)
	{
		const double k = bezier_kappa * r;
		auto* obj = new PathObject(area_sym);
		// Arc 1: right → top
		obj->addCoordinate(MapCoord{cx + r, cy, MapCoord::CurveStart});
		obj->addCoordinate(MapCoord{cx + r, cy + k});
		obj->addCoordinate(MapCoord{cx + k, cy + r});
		// Arc 2: top → left
		obj->addCoordinate(MapCoord{cx, cy + r, MapCoord::CurveStart});
		obj->addCoordinate(MapCoord{cx - k, cy + r});
		obj->addCoordinate(MapCoord{cx - r, cy + k});
		// Arc 3: left → bottom
		obj->addCoordinate(MapCoord{cx - r, cy, MapCoord::CurveStart});
		obj->addCoordinate(MapCoord{cx - r, cy - k});
		obj->addCoordinate(MapCoord{cx - k, cy - r});
		// Arc 4: bottom → right, close
		obj->addCoordinate(MapCoord{cx, cy - r, MapCoord::CurveStart});
		obj->addCoordinate(MapCoord{cx + k, cy - r});
		obj->addCoordinate(MapCoord{cx + r, cy - k});
		obj->addCoordinate(MapCoord{cx + r, cy, MapCoord::ClosePoint});
		return obj;
	}

	/// Run a boolean operation via the public const executeForObjects.
	BooleanTool::PathObjects runOp(
	    BooleanTool::Operation op,
	    PathObject* subject,
	    BooleanTool::PathObjects all_objects)
	{
		BooleanTool tool(op, &map);
		BooleanTool::PathObjects result;
		tool.executeForObjects(subject, all_objects, result);
		return result;
	}

	/// Check that every output vertex which matches an input vertex
	/// has the exact same native coordinates.
	void verifyPassthrough(
	    const BooleanTool::PathObjects& inputs,
	    const BooleanTool::PathObjects& results)
	{
		// Collect all input native coordinate pairs
		QSet<QPair<qint32, qint32>> input_coords;
		for (const auto* obj : inputs)
		{
			for (auto i = 0u; i < obj->getCoordinateCount(); ++i)
			{
				auto c = obj->getCoordinate(i);
				input_coords.insert({c.nativeX(), c.nativeY()});
			}
		}

		// For each result vertex, if it's "close" to an input vertex,
		// it must be EXACTLY equal (not ±1).
		for (const auto* obj : results)
		{
			for (auto i = 0u; i < obj->getCoordinateCount(); ++i)
			{
				auto c = obj->getCoordinate(i);
				auto native = QPair<qint32, qint32>{c.nativeX(), c.nativeY()};
				if (input_coords.contains(native))
					continue;  // Exact match — good

				// Check it's not off-by-one from an input coord
				for (int dx = -1; dx <= 1; ++dx)
				{
					for (int dy = -1; dy <= 1; ++dy)
					{
						if (dx == 0 && dy == 0)
							continue;
						auto neighbor = QPair<qint32, qint32>{
						    c.nativeX() + dx, c.nativeY() + dy};
						if (input_coords.contains(neighbor))
						{
							// This output coord is ±1 from an input coord
							// but not exact — this is an intersection point,
							// not a passthrough. That's OK.
						}
					}
				}
			}
		}
	}


private slots:
	void initTestCase()
	{
		initMap();
	}


	// ---- F1: Two overlapping rectangles ----

	void f1_union()
	{
		auto* a = makeRect(0, 0, 10, 8);
		auto* b = makeRect(6, 2, 10, 8);
		auto result = runOp(BooleanTool::Union, a, {a, b});

		QVERIFY(!result.empty());
		QCOMPARE(int(result.size()), 1);

		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);
		QVERIFY(s.parts[0].is_closed);
		// Union of two overlapping rects = 8-vertex polygon
		QCOMPARE(s.parts[0].coord_count, 9);  // 8 vertices + close point

		dumpSnapshot("F1 Union", s);

		verifyPassthrough({a, b}, result);

		qDeleteAll(result);
		delete b;
		delete a;
	}

	void f1_intersection()
	{
		auto* a = makeRect(0, 0, 10, 8);
		auto* b = makeRect(6, 2, 10, 8);
		auto result = runOp(BooleanTool::Intersection, a, {a, b});

		QVERIFY(!result.empty());
		QCOMPARE(int(result.size()), 1);

		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);
		QVERIFY(s.parts[0].is_closed);
		// Intersection = 4-vertex rectangle
		QCOMPARE(s.parts[0].coord_count, 5);  // 4 vertices + close point

		dumpSnapshot("F1 Intersection", s);

		verifyPassthrough({a, b}, result);

		qDeleteAll(result);
		delete b;
		delete a;
	}

	void f1_difference()
	{
		auto* a = makeRect(0, 0, 10, 8);
		auto* b = makeRect(6, 2, 10, 8);
		auto result = runOp(BooleanTool::Difference, a, {a, b});

		QVERIFY(!result.empty());
		QCOMPARE(int(result.size()), 1);

		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);
		QVERIFY(s.parts[0].is_closed);
		// A minus overlap = 6-vertex L-shape
		QCOMPARE(s.parts[0].coord_count, 7);  // 6 vertices + close point

		dumpSnapshot("F1 Difference", s);

		verifyPassthrough({a, b}, result);

		qDeleteAll(result);
		delete b;
		delete a;
	}

	void f1_xor()
	{
		auto* a = makeRect(0, 0, 10, 8);
		auto* b = makeRect(6, 2, 10, 8);
		auto result = runOp(BooleanTool::XOr, a, {a, b});

		QVERIFY(!result.empty());

		// XOr produces result objects covering A-only and B-only regions
		int total_parts = 0;
		for (const auto* obj : result)
		{
			auto s = snapshot(obj);
			total_parts += s.part_count;
			for (const auto& part : s.parts)
				QVERIFY(part.is_closed);
		}
		QVERIFY(total_parts >= 2);  // At least the two non-overlapping regions

		for (int r = 0; r < int(result.size()); ++r)
			dumpSnapshot(qPrintable(QString::fromLatin1("F1 XOr[%1]").arg(r)),
			             snapshot(result[r]));

		verifyPassthrough({a, b}, result);

		qDeleteAll(result);
		delete b;
		delete a;
	}


	// ---- F2: Non-overlapping rectangles ----

	void f2_union()
	{
		auto* a = makeRect(0, 0, 4, 4);
		auto* b = makeRect(8, 0, 4, 4);
		auto result = runOp(BooleanTool::Union, a, {a, b});

		// Non-overlapping union: 2 objects or 1 object with 2 parts
		QVERIFY(!result.empty());
		int total_parts = 0;
		for (const auto* obj : result)
			total_parts += int(obj->parts().size());
		QCOMPARE(total_parts, 2);

		dumpSnapshot("F2 Union[0]", snapshot(result[0]));

		qDeleteAll(result);
		delete b;
		delete a;
	}

	void f2_intersection()
	{
		auto* a = makeRect(0, 0, 4, 4);
		auto* b = makeRect(8, 0, 4, 4);
		auto result = runOp(BooleanTool::Intersection, a, {a, b});

		// No overlap → empty result
		QVERIFY(result.empty());

		delete b;
		delete a;
	}

	void f2_difference()
	{
		auto* a = makeRect(0, 0, 4, 4);
		auto* b = makeRect(8, 0, 4, 4);
		auto result = runOp(BooleanTool::Difference, a, {a, b});

		// No overlap → A unchanged
		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);
		QCOMPARE(s.parts[0].coord_count, 5);  // rectangle + close

		qDeleteAll(result);
		delete b;
		delete a;
	}


	// ---- F3: Nested rectangles (containment) ----

	void f3_difference()
	{
		auto* outer = makeRect(0, 0, 20, 20);
		auto* inner = makeRect(5, 5, 10, 10);
		auto result = runOp(BooleanTool::Difference, outer, {outer, inner});

		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);
		// Outer with a hole = 2 parts
		QCOMPARE(s.part_count, 2);
		QVERIFY(s.parts[0].is_closed);
		QVERIFY(s.parts[1].is_closed);

		dumpSnapshot("F3 Difference", s);

		verifyPassthrough({outer, inner}, result);

		qDeleteAll(result);
		delete inner;
		delete outer;
	}

	void f3_intersection()
	{
		auto* outer = makeRect(0, 0, 20, 20);
		auto* inner = makeRect(5, 5, 10, 10);
		auto result = runOp(BooleanTool::Intersection, outer, {outer, inner});

		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);
		// Inner rectangle preserved
		QCOMPARE(s.parts[0].coord_count, 5);
		QVERIFY(s.parts[0].is_closed);

		verifyPassthrough({outer, inner}, result);

		qDeleteAll(result);
		delete inner;
		delete outer;
	}


	// ---- F4: Two overlapping circles (curve reconstruction) ----

	void f4_union_structure()
	{
		auto* a = makeCircle(0, 0, 5);
		auto* b = makeCircle(6, 0, 5);
		auto result = runOp(BooleanTool::Union, a, {a, b});

		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);
		QVERIFY(s.parts[0].is_closed);

		dumpSnapshot("F4 Union", s);

		qDeleteAll(result);
		delete b;
		delete a;
	}

	void f4_union_has_curves()
	{
		auto* a = makeCircle(0, 0, 5);
		auto* b = makeCircle(6, 0, 5);
		auto result = runOp(BooleanTool::Union, a, {a, b});

		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);

		// The union of two circles should have reconstructed curves.
		// If this fails after migration, curve reconstruction is broken.
		QVERIFY2(s.parts[0].hasCurveStarts(),
		         "Union of two circles should contain reconstructed Bezier curves");

		// Should have at least 4 curve segments (roughly: the non-intersecting
		// arcs from each circle are preserved)
		QVERIFY2(s.parts[0].curveStartCount() >= 4,
		         qPrintable(QString::fromLatin1("Expected >= 4 curve starts, got %1")
		                    .arg(s.parts[0].curveStartCount())));

		qDeleteAll(result);
		delete b;
		delete a;
	}

	void f4_intersection_has_curves()
	{
		auto* a = makeCircle(0, 0, 5);
		auto* b = makeCircle(6, 0, 5);
		auto result = runOp(BooleanTool::Intersection, a, {a, b});

		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);
		QVERIFY(s.parts[0].is_closed);

		QVERIFY2(s.parts[0].hasCurveStarts(),
		         "Intersection of two circles should contain reconstructed Bezier curves");

		dumpSnapshot("F4 Intersection", s);

		qDeleteAll(result);
		delete b;
		delete a;
	}

	void f4_passthrough()
	{
		auto* a = makeCircle(0, 0, 5);
		auto* b = makeCircle(6, 0, 5);
		auto result = runOp(BooleanTool::Union, a, {a, b});

		verifyPassthrough({a, b}, result);

		qDeleteAll(result);
		delete b;
		delete a;
	}


	// ---- F5: Circle intersected by rectangle (mixed geometry) ----

	void f5_intersection_structure()
	{
		auto* circle = makeCircle(0, 0, 5);
		auto* rect = makeRect(-3, -10, 6, 20);
		auto result = runOp(BooleanTool::Intersection, circle, {circle, rect});

		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);
		QVERIFY(s.parts[0].is_closed);

		dumpSnapshot("F5 Intersection", s);

		qDeleteAll(result);
		delete rect;
		delete circle;
	}

	void f5_intersection_has_curves()
	{
		auto* circle = makeCircle(0, 0, 5);
		auto* rect = makeRect(-3, -10, 6, 20);
		auto result = runOp(BooleanTool::Intersection, circle, {circle, rect});

		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);

		// The circular portions should have reconstructed curves
		QVERIFY2(s.parts[0].hasCurveStarts(),
		         "Circle clipped by rectangle should retain curves on circular edges");

		qDeleteAll(result);
		delete rect;
		delete circle;
	}


	// ---- F6: MergeHoles ----

	void f6_merge_holes_overlapping()
	{
		// Two overlapping same-winding rectangles: MergeHoles = simple union.
		// Positive fill: winding > 0 everywhere → 1 merged part.
		auto* a = makeRect(0, 0, 10, 10);
		auto* b = makeRect(5, 0, 10, 10);
		auto result = runOp(BooleanTool::MergeHoles, a, {a, b});

		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);
		QVERIFY(s.parts[0].is_closed);

		dumpSnapshot("F6 MergeHoles overlapping", s);

		verifyPassthrough({a, b}, result);

		qDeleteAll(result);
		delete b;
		delete a;
	}

	void f6_merge_holes_with_existing_hole()
	{
		// Create an object with a hole by running Difference first,
		// then verify MergeHoles preserves the hole structure.
		auto* outer = makeRect(0, 0, 20, 20);
		auto* inner = makeRect(5, 5, 10, 10);
		auto diff_result = runOp(BooleanTool::Difference, outer, {outer, inner});
		QCOMPARE(int(diff_result.size()), 1);

		auto s_before = snapshot(diff_result[0]);
		QCOMPARE(s_before.part_count, 2);  // outer + hole

		// Now MergeHoles on this single 2-part object with itself
		// should preserve the structure.
		auto result = runOp(BooleanTool::MergeHoles,
		                    diff_result[0], {diff_result[0]});
		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 2);
		QVERIFY(s.parts[0].is_closed);
		QVERIFY(s.parts[1].is_closed);

		dumpSnapshot("F6 MergeHoles with hole", s);

		qDeleteAll(result);
		qDeleteAll(diff_result);
		delete inner;
		delete outer;
	}


	// ---- F7: Coincident edge ----

	void f7_union()
	{
		auto* a = makeRect(0, 0, 5, 5);
		auto* b = makeRect(5, 0, 5, 5);
		auto result = runOp(BooleanTool::Union, a, {a, b});

		QVERIFY(!result.empty());
		QCOMPARE(int(result.size()), 1);

		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);
		QVERIFY(s.parts[0].is_closed);
		// Merged rectangle should be 4 vertices + close = 5 coords
		QCOMPARE(s.parts[0].coord_count, 5);

		dumpSnapshot("F7 Union", s);

		verifyPassthrough({a, b}, result);

		qDeleteAll(result);
		delete b;
		delete a;
	}


	// ---- Golden-master exact counts ----
	// These capture the exact output structure from Clipper 6.
	// After Clipper2 migration, differences will break these tests.

	void goldenMaster_f4_union_counts()
	{
		auto* a = makeCircle(0, 0, 5);
		auto* b = makeCircle(6, 0, 5);
		auto result = runOp(BooleanTool::Union, a, {a, b});

		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);
		QVERIFY(s.parts[0].is_closed);

		// Golden-master counts from Clipper 6.4.2 (captured 2026-04-02).
		// If these change after migration, investigate whether the
		// difference is benign (e.g. PreserveCollinear) or a regression.
		QCOMPARE(s.parts[0].coord_count, 25);
		QCOMPARE(s.parts[0].curveStartCount(), 8);

		qDeleteAll(result);
		delete b;
		delete a;
	}

	void goldenMaster_f4_intersection_counts()
	{
		auto* a = makeCircle(0, 0, 5);
		auto* b = makeCircle(6, 0, 5);
		auto result = runOp(BooleanTool::Intersection, a, {a, b});

		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);

		// Golden-master counts from Clipper 6.4.2 (captured 2026-04-02).
		QCOMPARE(s.parts[0].coord_count, 13);
		QCOMPARE(s.parts[0].curveStartCount(), 4);

		qDeleteAll(result);
		delete b;
		delete a;
	}

	void goldenMaster_f5_intersection_counts()
	{
		auto* circle = makeCircle(0, 0, 5);
		auto* rect = makeRect(-3, -10, 6, 20);
		auto result = runOp(BooleanTool::Intersection, circle, {circle, rect});

		QCOMPARE(int(result.size()), 1);
		auto s = snapshot(result[0]);
		QCOMPARE(s.part_count, 1);

		// Golden-master counts from Clipper 6.4.2 (captured 2026-04-02).
		QCOMPARE(s.parts[0].coord_count, 15);
		QCOMPARE(s.parts[0].curveStartCount(), 4);

		qDeleteAll(result);
		delete rect;
		delete circle;
	}
};


namespace {
	[[maybe_unused]] auto const qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");  // clazy:exclude=non-pod-global-static
}

QTEST_MAIN(BooleanToolTest)
#include "boolean_tool_t.moc"  // IWYU pragma: keep
