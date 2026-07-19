/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2015-2020, 2025 Kai Pastor
 *    Copyright 2025 Matthias Kühlewein
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


#include "tools_t.h"

#include <cmath>
#include <memory>

#include <Qt>
#include <QtGlobal>
#include <QtTest>
#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPoint>
#include <QPointF>
#include <QString>

#include "core/map.h"
#include "core/map_color.h"
#include "core/map_coord.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "core/objects/object_query.h"
#include "core/objects/text_object.h"
#include "core/symbols/area_symbol.h"
#include "core/symbols/line_symbol.h"
#include "core/symbols/point_symbol.h"
#include "core/symbols/text_symbol.h"
#include "global.h"
#include "gui/main_window.h"
#include "gui/map/map_editor.h"
#include "gui/map/map_find_feature.h"
#include "gui/map/map_widget.h"
#include "templates/paint_on_template_feature.h"
#include "templates/template_image.h"
#include "tools/edit_point_tool.h"
#include "tools/edit_tool.h"
#include "undo/undo_manager.h"

using namespace OpenOrienteering;

namespace {

class FrameContextRasterTemplate final : public TemplateImage
{
public:
	explicit FrameContextRasterTemplate(Map* map)
	 : TemplateImage(QString(), map)
	{
		image = QImage(4, 4, QImage::Format_RGBA8888);
		image.fill(Qt::blue);
		setTemplateState(Loaded);
	}

	void arm()
	{
		armed = true;
		context_seen = false;
		collected = false;
		collected_without_context = false;
	}

	void updateRenderContext(const ViewRenderContext&) override
	{
		if (armed)
			context_seen = true;
	}

	void collectRasterTiles(const QRectF&, double, bool,
	                        QVector<RasterTemplateTile>& out) const override
	{
		if (armed)
		{
			collected = true;
			collected_without_context = !context_seen;
			armed = false;
		}
		out.push_back({
			image,
			QRectF(-2, -2, 4, 4),
			QRectF(0, 0, 4, 4),
			quint64(image.cacheKey()),
			false,
		});
	}

	mutable bool armed = false;
	mutable bool context_seen = false;
	mutable bool collected = false;
	mutable bool collected_without_context = false;
};

} // namespace


/// Creates a test map and provides pointers to specific map elements.
/// NOTE: delete the map manually in case its ownership is not transferred to a MapEditorController or similar!
struct TestMap
{
	Map* map;
	
	LineSymbol* line_symbol;
	PathObject* line_object;
	
	TestMap();
	~TestMap();
};


/// Creates a test map editor and provides related pointers.
struct TestMapEditor
{
	MainWindow* window;
	MapEditorController* editor;
	MapWidget* map_widget;
	
	TestMapEditor(Map* map);
	TestMapEditor(const TestMapEditor&) = delete;
	TestMapEditor& operator=(const TestMapEditor&) = delete;
	~TestMapEditor();
	
	void simulateClick(const QPoint& pos);
	void simulateClick(const QPointF& pos);
	void simulateDrag(const QPoint& start_pos, const QPoint& end_pos);
	void simulateDrag(const QPointF& start_pos, const QPointF& end_pos);
};


// ### TestMap ###

TestMap::TestMap()
{
	MapCoord coord;
	
	map = new Map();
	
	auto black = new MapColor();
	black->setCmyk(MapColorCmyk(0.0f, 0.0f, 0.0f, 1.0f));
	black->setOpacity(1.0f);
	black->setName(QString::fromLatin1("black"));
	map->addColor(black, 0);
	
	line_symbol = new LineSymbol();
	line_symbol->setLineWidth(1);
	line_symbol->setColor(black);
	map->addSymbol(line_symbol, 0);
	
	line_object = new PathObject(line_symbol);
	line_object->addCoordinate(MapCoord(10, 10));
	coord = MapCoord(20, 10);
	coord.setCurveStart(true);
	line_object->addCoordinate(coord);
	line_object->addCoordinate(MapCoord(20, 20));
	line_object->addCoordinate(MapCoord(30, 20));
	line_object->addCoordinate(MapCoord(30, 10));
	map->addObject(line_object);
	
	// TODO: fill map with more content as needed
}

TestMap::~TestMap() = default;


// ### TestMapEditor ###

TestMapEditor::TestMapEditor(Map* map)
{
	window = new MainWindow();
	editor = new MapEditorController(MapEditorController::MapEditor, map);
	window->setController(editor);
	map_widget = editor->getMainWidget();
}

TestMapEditor::~TestMapEditor()
{
	// The window may still be referred to by tools which are scheduled for
	// deleteLater(), so we need to postpone the window deletion, too.
	window->deleteLater();
}

void TestMapEditor::simulateClick(const QPoint& pos)
{
	QTest::mouseClick(map_widget, Qt::LeftButton, {}, pos);
}

void TestMapEditor::simulateClick(const QPointF& pos)
{
	simulateClick(pos.toPoint());
}

void TestMapEditor::simulateDrag(const QPoint& start_pos, const QPoint& end_pos)
{
	QTest::mousePress(map_widget, Qt::LeftButton, {}, start_pos);
	
	// NOTE: the implementation of QTest::mouseMove() does not seem to work (tries to set the real cursor position ...)
	//QTest::mouseMove(map_widget, end_pos);
	// Use manual workaround instead which sends an event directly:
	QMouseEvent event(QEvent::MouseMove, end_pos, map_widget->mapToGlobal(end_pos), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(map_widget, &event);
	
	QTest::mouseRelease(map_widget, Qt::LeftButton, {}, end_pos);
}

void TestMapEditor::simulateDrag(const QPointF& start_pos, const QPointF& end_pos)
{
	simulateDrag(start_pos.toPoint(), end_pos.toPoint());
}


// ### ToolsTest ###

void ToolsTest::initTestCase()
{
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
}


void ToolsTest::newMapStartsWithoutFormat()
{
	auto* map = new Map;
	TestMapEditor editor(map);  // taking ownership
	QCOMPARE(editor.window->currentFormat(), nullptr);
}

void ToolsTest::framePublishesTemplateContextBeforeRasterCollection()
{
	auto* map = new Map;
	auto raster = std::make_unique<FrameContextRasterTemplate>(map);
	auto* raster_ptr = raster.get();
	map->addTemplate(0, std::move(raster));
	TestMapEditor editor(map); // taking ownership
	editor.map_widget->resize(320, 240);
	auto* view = editor.map_widget->getMapView();
	view->setTemplateVisibility(raster_ptr, { 1, true });
	QTest::qWait(20);

	raster_ptr->arm();
	view->setZoom(view->getZoom() * std::sqrt(2.0));
	QTRY_VERIFY_WITH_TIMEOUT(raster_ptr->collected, 1000);
	QVERIFY(!raster_ptr->collected_without_context);
}


void ToolsTest::editTool()
{
	// Initialization
	TestMap map;
	TestMapEditor editor(map.map);
	EditTool* tool = new EditPointTool(editor.editor, nullptr);	// TODO: Refactor EditTool: MapEditorController and SymbolWidget pointers could be unnecessary
	editor.editor->setTool(tool);
	
	// Move the first coordinate of the line object
	const MapWidget* map_widget = editor.map_widget;
	const PathObject* object = map.line_object;
	
	QPointF drag_start_pos = map_widget->mapToViewport(object->getCoordinate(0));
	QPointF drag_end_pos = drag_start_pos + QPointF(0, -50);
	
	// Clear selection.
	map.map->clearObjectSelection(false);
	QVERIFY(map.map->selectedObjects().empty());
	
	// Click to select the object
	editor.simulateClick(drag_start_pos);
	QCOMPARE(map.map->getFirstSelectedObject(), object);
	
	// Drag the coordinate to the new position
	editor.simulateDrag(drag_start_pos, drag_end_pos);
	
	// Check position deviation
	QPointF difference = map_widget->mapToViewport(object->getCoordinate(0)) - drag_end_pos;
	QCOMPARE(qMax(qAbs(difference.x()), 0.1), 0.1);
	QCOMPARE(qMax(qAbs(difference.y()), 0.1), 0.1);
	
	// Cleanup
	editor.editor->setTool(nullptr);
}


void ToolsTest::arrowKeysNudgeSelection()
{
	TestMap map;
	TestMapEditor editor(map.map);
	auto* const black = map.map->getColor(0);

	auto* point_symbol = new PointSymbol();
	map.map->addSymbol(point_symbol, map.map->getNumSymbols());
	auto* point = new PointObject(point_symbol);
	point->setPosition(200, 300);
	map.map->addObject(point);

	auto* area_symbol = new AreaSymbol();
	area_symbol->setColor(black);
	map.map->addSymbol(area_symbol, map.map->getNumSymbols());
	auto* area = new PathObject(area_symbol);
	area->addCoordinate(MapCoord::fromNative(100, 100));
	area->addCoordinate(MapCoord::fromNative(100, 400));
	area->addCoordinate(MapCoord::fromNative(400, 400));
	area->addCoordinate(MapCoord::fromNative(400, 100));
	area->addCoordinate(MapCoord::fromNative(100, 100, MapCoord::ClosePoint));
	map.map->addObject(area);

	auto* text_symbol = new TextSymbol();
	text_symbol->setColor(black);
	map.map->addSymbol(text_symbol, map.map->getNumSymbols());
	auto* text = new TextObject(text_symbol);
	text->setBox(500, 600, 2.0, 3.0);
	text->setText(QLatin1String("Nudge"));
	map.map->addObject(text);
	const auto text_box_size = text->getBoxSize();

	map.map->clearObjectSelection(false);
	map.map->addObjectToSelection(map.line_object, false);
	map.map->addObjectToSelection(point, false);
	map.map->addObjectToSelection(area, false);
	map.map->addObjectToSelection(text, true);
	QCOMPARE(map.map->getNumSelectedObjects(), 4);
	editor.editor->setTool(new EditPointTool(editor.editor, nullptr));

	auto* const view = editor.map_widget->getMapView();
	view->setZoom(8.0);
	view->setRotation(0.61);

	std::vector<MapCoordVector> original_coords;
	for (int i = 0; i < map.map->getCurrentPart()->getNumObjects(); ++i)
		original_coords.push_back(map.map->getCurrentPart()->getObject(i)->getRawCoordinateVector());

	const auto point_before = editor.map_widget->mapToViewport(point->getRawCoordinateVector()[0]);
	const auto undo_count = map.map->undoManager().undoStepCount();
	QKeyEvent right_press(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
	QVERIFY(editor.map_widget->keyPressEventFilter(&right_press));
	QKeyEvent right_release(QEvent::KeyRelease, Qt::Key_Right, Qt::NoModifier);
	QVERIFY(!editor.map_widget->keyReleaseEventFilter(&right_release));

	const auto point_after_right = editor.map_widget->mapToViewport(point->getRawCoordinateVector()[0]);
	QVERIFY(qAbs(point_after_right.x() - point_before.x() - 1.0) < 0.1);
	QVERIFY(qAbs(point_after_right.y() - point_before.y()) < 0.1);
	QCOMPARE(text->getBoxSize(), text_box_size);
	QCOMPARE(map.map->undoManager().undoStepCount(), undo_count + 1);

	const auto dx = point->getRawCoordinateVector()[0].nativeX() - original_coords[1][0].nativeX();
	const auto dy = point->getRawCoordinateVector()[0].nativeY() - original_coords[1][0].nativeY();
	QVERIFY(dx != 0 || dy != 0);
	for (int object_index = 0; object_index < map.map->getCurrentPart()->getNumObjects(); ++object_index)
	{
		const auto* object = map.map->getCurrentPart()->getObject(object_index);
		const auto& before = original_coords[std::size_t(object_index)];
		const auto& after = object->getRawCoordinateVector();
		QCOMPARE(after.size(), before.size());
		for (MapCoordVector::size_type coord_index = 0; coord_index < before.size(); ++coord_index)
		{
			auto expected = before[coord_index];
			expected.setNativeX(expected.nativeX() + dx);
			expected.setNativeY(expected.nativeY() + dy);
			QCOMPARE(after[coord_index], expected);
		}
	}

	const auto point_before_coarse = point_after_right;
	QKeyEvent up_press(QEvent::KeyPress, Qt::Key_Up, Qt::ShiftModifier);
	QVERIFY(editor.map_widget->keyPressEventFilter(&up_press));
	QKeyEvent up_release(QEvent::KeyRelease, Qt::Key_Up, Qt::ShiftModifier);
	QVERIFY(!editor.map_widget->keyReleaseEventFilter(&up_release));
	const auto point_after_coarse = editor.map_widget->mapToViewport(point->getRawCoordinateVector()[0]);
	QVERIFY(qAbs(point_after_coarse.x() - point_before_coarse.x()) < 0.1);
	QVERIFY(qAbs(point_after_coarse.y() - point_before_coarse.y() + 10.0) < 0.1);
	QCOMPARE(map.map->undoManager().undoStepCount(), undo_count + 2);

	QVERIFY(map.map->undoManager().undo());
	QVERIFY(map.map->undoManager().undo());
	for (int object_index = 0; object_index < map.map->getCurrentPart()->getNumObjects(); ++object_index)
		QCOMPARE(map.map->getCurrentPart()->getObject(object_index)->getRawCoordinateVector(),
		         original_coords[std::size_t(object_index)]);

	QVERIFY(map.map->undoManager().redo());
	QVERIFY(map.map->undoManager().redo());
	editor.editor->setTool(nullptr);
}


void ToolsTest::arrowKeyAutoRepeatIsSingleUndoStep()
{
	TestMap map;
	TestMapEditor editor(map.map);
	map.map->addObjectToSelection(map.line_object, true);

	const auto original_coord = map.line_object->getCoordinate(0);
	const auto viewport_before = editor.map_widget->mapToViewport(original_coord);
	const auto undo_count = map.map->undoManager().undoStepCount();

	QKeyEvent initial_press(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
	QVERIFY(editor.map_widget->keyPressEventFilter(&initial_press));
	for (int i = 0; i < 2; ++i)
	{
		QKeyEvent repeat_press(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier, QString{}, true, 1);
		QVERIFY(editor.map_widget->keyPressEventFilter(&repeat_press));
	}
	QKeyEvent final_release(QEvent::KeyRelease, Qt::Key_Right, Qt::NoModifier);
	QVERIFY(!editor.map_widget->keyReleaseEventFilter(&final_release));

	const auto viewport_after = editor.map_widget->mapToViewport(map.line_object->getCoordinate(0));
	QVERIFY(qAbs(viewport_after.x() - viewport_before.x() - 3.0) < 0.1);
	QVERIFY(qAbs(viewport_after.y() - viewport_before.y()) < 0.1);
	QCOMPARE(map.map->undoManager().undoStepCount(), undo_count + 1);

	QVERIFY(map.map->undoManager().undo());
	QCOMPARE(map.map->getCurrentPart()->getObject(0)->getRawCoordinateVector()[0], original_coord);
	QVERIFY(map.map->undoManager().redo());
}


void ToolsTest::arrowKeysPanWithoutSelection()
{
	TestMap map;
	TestMapEditor editor(map.map);
	QVERIFY(map.map->selectedObjects().empty());

	const auto object_coord = map.line_object->getCoordinate(0);
	const auto center = editor.map_widget->getMapView()->center();
	QKeyEvent right_press(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
	QVERIFY(editor.map_widget->keyPressEventFilter(&right_press));

	QCOMPARE(map.line_object->getCoordinate(0), object_coord);
	QVERIFY(editor.map_widget->getMapView()->center() != center);
}



void ToolsTest::paintOnTemplateFeature()
{
	// Designed for images of 100 mm at 10 pixel per mm.
	QCOMPARE(PaintOnTemplateFeature::alignmentBase(20000), 200);
	QCOMPARE(PaintOnTemplateFeature::alignmentBase(15000), 100);
	QCOMPARE(PaintOnTemplateFeature::alignmentBase(10000), 100);
	QCOMPARE(PaintOnTemplateFeature::alignmentBase(7500) , 100);
	QCOMPARE(PaintOnTemplateFeature::alignmentBase(5000) , 50);
	QCOMPARE(PaintOnTemplateFeature::alignmentBase(4000) , 50);
	QCOMPARE(PaintOnTemplateFeature::alignmentBase(1000) , 10);
	
	QCOMPARE(PaintOnTemplateFeature::roundToMultiple(-347,  10), -350);
	QCOMPARE(PaintOnTemplateFeature::roundToMultiple(-347,  20), -340);
	QCOMPARE(PaintOnTemplateFeature::roundToMultiple(-347,  50), -350);
	QCOMPARE(PaintOnTemplateFeature::roundToMultiple(-347, 100), -300);
	QCOMPARE(PaintOnTemplateFeature::roundToMultiple(347,  10), 350);
	QCOMPARE(PaintOnTemplateFeature::roundToMultiple(347,  20), 340);
	QCOMPARE(PaintOnTemplateFeature::roundToMultiple(347,  50), 350);
	QCOMPARE(PaintOnTemplateFeature::roundToMultiple(347, 100), 300);
}


void ToolsTest::testFindObjects()
{
	auto* map = new Map;
	{
		auto* normal_point_symbol = new PointSymbol();
		map->addSymbol(normal_point_symbol, 0);
		
		auto* hidden_point_symbol = new PointSymbol();
		hidden_point_symbol->setHidden(true);
		map->addSymbol(hidden_point_symbol, 1);
		
		auto* protected_point_symbol = new PointSymbol();
		protected_point_symbol->setProtected(true);
		map->addSymbol(protected_point_symbol, 2);
		
		auto add_object = [map](Symbol* symbol, const char* label) {
			auto* object = new PointObject(symbol);
			object->setTag(QLatin1String("match"), QLatin1String(label));
			map->addObject(object);
		};
		add_object(normal_point_symbol, "yes");     // expected match
		add_object(normal_point_symbol, "no");
		add_object(normal_point_symbol, "yes");     // expected match
		add_object(hidden_point_symbol, "yes");
		add_object(normal_point_symbol, "yes");     // expected match
		add_object(protected_point_symbol, "yes");
	}
	
	TestMapEditor editor(map);  // taking ownership
	
	ObjectQuery query {QLatin1String("match"), ObjectQuery::OperatorIs, QLatin1String("yes")};
	QVERIFY(query);
	
	MapFindFeature::findAllMatchingObjects(*editor.editor, query);
	QCOMPARE(map->getNumSelectedObjects(), 3);
	
	MapFindFeature::findNextMatchingObject(*editor.editor, query);
	QCOMPARE(map->getNumSelectedObjects(), 1);
	auto* first_match = map->getFirstSelectedObject();
	
	MapFindFeature::findNextMatchingObject(*editor.editor, query);
	QCOMPARE(map->getNumSelectedObjects(), 1);
	QVERIFY(map->getFirstSelectedObject() != first_match);
	
	MapFindFeature::findNextMatchingObject(*editor.editor, query);
	QCOMPARE(map->getNumSelectedObjects(), 1);
	QVERIFY(map->getFirstSelectedObject() != first_match);
	
	MapFindFeature::findNextMatchingObject(*editor.editor, query);
	QCOMPARE(map->getNumSelectedObjects(), 1);
	QVERIFY(map->getFirstSelectedObject() == first_match);
}


/*
 * We select a non-standard QPA because we don't need a real GUI window.
 */
namespace  {
	[[maybe_unused]] const auto qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");  // clazy:exclude=non-pod-global-static
}


QTEST_MAIN(ToolsTest)
