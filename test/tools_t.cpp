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
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPoint>
#include <QPointF>
#include <QStackedWidget>
#include <QString>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include "core/map.h"
#include "core/map_color.h"
#include "core/map_coord.h"
#include "core/objects/object.h"
#include "core/objects/object_query.h"
#include "core/sketch_layer.h"
#include "core/symbols/line_symbol.h"
#include "core/symbols/point_symbol.h"
#include "global.h"
#include "settings.h"
#include "gui/main_window.h"
#include "gui/map/map_editor.h"
#include "gui/map/map_find_feature.h"
#include "gui/map/map_widget.h"
#include "templates/template_image.h"
#include "tools/edit_point_tool.h"
#include "tools/edit_tool.h"
#include "tools/pan_tool.h"
#include "tools/sketch_tool.h"
#include "undo/undo.h"
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

	void updateRenderContext(const ViewRenderContext& context) override
	{
		if (context.demand == ViewRenderContext::Demand::Coverage)
			++coverage_context_count;
		else
			++full_context_count;
		if (armed)
			context_seen = true;
	}

	void collectRasterTiles(const QRectF&, double, bool,
	                        QVector<RasterTemplateTile>& out) const override
	{
		++collection_count;
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
	mutable int collection_count = 0;
	int coverage_context_count = 0;
	int full_context_count = 0;
};

QAction* actionWithText(QObject* parent, const QString& text)
{
	for (auto* action : parent->findChildren<QAction*>())
	{
		if (action->text() == text)
			return action;
	}
	return nullptr;
}

void acceptFirstSketchLayer()
{
	QTimer::singleShot(0, [] {
		auto* dialog = qobject_cast<QDialog*>(
		    QApplication::activeModalWidget());
		QVERIFY(dialog);
		auto* list = dialog->findChild<QListWidget*>();
		QVERIFY(list);
		list->setCurrentRow(0);
		dialog->accept();
	});
}

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

void ToolsTest::unattachedLoadedEditorDestructsCleanly()
{
	auto* map = new Map;
	map->undoManager().push(
	  std::make_unique<NoOpUndoStep>(map, true));

	// File-provider reloads construct and load a replacement controller before
	// attaching it to a window.  Destroying a superseded replacement must not
	// send UndoManager teardown signals to actions which do not exist yet.
	delete new MapEditorController(MapEditorController::MapEditor, map);
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

void ToolsTest::mobileSecondaryPagesReplaceMapSurface()
{
	auto& settings = Settings::getInstance();
	const auto previous_touch_mode = settings.touchModeEnabled();
	settings.setTouchModeEnabled(true);
	{
		auto* map = new Map;
		map->addSymbol(new PointSymbol, 0);
		TestMapEditor editor(map); // taking ownership
		editor.window->resize(844, 390);
		editor.window->show();
		QCoreApplication::processEvents();
		auto* pages = editor.window->findChild<QStackedWidget*>(
		  QStringLiteral("mobileEditorPages"));
		auto* symbol_button = editor.window->findChild<QToolButton*>(
		  QStringLiteral("mobileSymbolPickerButton"));
		QVERIFY(pages);
		QVERIFY(symbol_button);
		QVERIFY(symbol_button->isEnabled());
		QVERIFY(!symbol_button->menu());
		QCOMPARE(pages->currentWidget()->objectName(), QString{});

		symbol_button->click();
		QCOMPARE(pages->currentWidget()->objectName(),
		         QStringLiteral("mobileSymbolPage"));
		QVERIFY(pages->currentWidget() != editor.map_widget);

		editor.editor->mobileSymbolSelectorFinished();
		QCOMPARE(pages->currentIndex(), 0);
		editor.editor->showMobileGnssDetails();
		QCOMPARE(pages->currentWidget()->objectName(),
		         QStringLiteral("mobileGnssPage"));
		editor.editor->showMobileMapPage();
		QCOMPARE(pages->currentIndex(), 0);

		editor.editor->setReadOnly(true);
		QVERIFY(symbol_button->isEnabled());
		symbol_button->click();
		QCOMPARE(pages->currentWidget()->objectName(),
		         QStringLiteral("mobileSymbolPage"));
		auto* read_only_label = editor.window->findChild<QLabel*>(
		  QStringLiteral("mobileSymbolReadOnlyLabel"));
		QVERIFY(read_only_label);
		QVERIFY(read_only_label->isVisible());
		editor.editor->mobileSymbolSelectorFinished();
		QCOMPARE(pages->currentIndex(), 0);
	}
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	settings.setTouchModeEnabled(previous_touch_mode);
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

void ToolsTest::panToolRecoversFromLostRelease()
{
	auto* map = new Map;
	TestMapEditor editor(map);
	editor.map_widget->resize(320, 240);
	editor.editor->setTool(new PanTool(editor.editor, nullptr));
	auto* view = editor.map_widget->getMapView();
	auto const initial_center = view->center();

	auto send_mouse = [&](QEvent::Type type, const QPoint& position,
	                      Qt::MouseButton button, Qt::MouseButtons buttons) {
		QMouseEvent event(type, QPointF(position),
		                  QPointF(editor.map_widget->mapToGlobal(position)),
		                  button, buttons, Qt::NoModifier);
		QApplication::sendEvent(editor.map_widget, &event);
	};

	// Reproduce a native-surface sequence where the first pan's release is
	// lost.  The following press must commit the visible offset and establish
	// a fresh drag instead of asserting in startDragging().
	send_mouse(QEvent::MouseButtonPress, {80, 120},
	           Qt::LeftButton, Qt::LeftButton);
	send_mouse(QEvent::MouseMove, {130, 120},
	           Qt::NoButton, Qt::LeftButton);
	QCOMPARE(view->panOffset(), QPoint(50, 0));

	send_mouse(QEvent::MouseButtonPress, {130, 120},
	           Qt::LeftButton, Qt::LeftButton);
	QCOMPARE(view->panOffset(), QPoint());
	auto const center_after_recovery = view->center();
	QVERIFY(center_after_recovery != initial_center);

	send_mouse(QEvent::MouseMove, {180, 120},
	           Qt::NoButton, Qt::LeftButton);
	QCOMPARE(view->panOffset(), QPoint(50, 0));
	send_mouse(QEvent::MouseButtonRelease, {180, 120},
	           Qt::LeftButton, Qt::NoButton);
	QCOMPARE(view->panOffset(), QPoint());
	QVERIFY(view->center() != center_after_recovery);
}

void ToolsTest::panDefersTemplateSceneIntegration()
{
	auto* map = new Map;
	auto raster = std::make_unique<FrameContextRasterTemplate>(map);
	auto* raster_ptr = raster.get();
	map->addTemplate(0, std::move(raster));
	TestMapEditor editor(map);
	editor.map_widget->resize(320, 240);
	auto* view = editor.map_widget->getMapView();
	view->setTemplateVisibility(raster_ptr, { 1, true });
	QTRY_VERIFY_WITH_TIMEOUT(raster_ptr->collection_count > 0, 1000);
	editor.editor->setTool(new PanTool(editor.editor, nullptr));
	QTest::qWait(20);
	auto const resting_collections = raster_ptr->collection_count;
	auto const resting_coverage_contexts = raster_ptr->coverage_context_count;
	auto const resting_full_contexts = raster_ptr->full_context_count;

	QTest::mousePress(editor.map_widget, Qt::LeftButton, {}, QPoint(80, 120));
	QMouseEvent move(
		QEvent::MouseMove, QPointF(140, 120),
		QPointF(editor.map_widget->mapToGlobal(QPoint(140, 120))),
		Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(editor.map_widget, &move);
	map->setTemplateAreaDirty(raster_ptr, QRectF(-10, -10, 20, 20), 0);
	QTRY_VERIFY_WITH_TIMEOUT(
		raster_ptr->coverage_context_count > resting_coverage_contexts, 1000);
	QCOMPARE(raster_ptr->collection_count, resting_collections);
	QCOMPARE(raster_ptr->full_context_count, resting_full_contexts);

	QTest::mouseRelease(
		editor.map_widget, Qt::LeftButton, {}, QPoint(140, 120));
	QTRY_VERIFY_WITH_TIMEOUT(
		raster_ptr->collection_count > resting_collections, 1000);
	QVERIFY(raster_ptr->full_context_count > resting_full_contexts);
}

void ToolsTest::wheelDefersTemplateSceneIntegrationUntilIdle()
{
	auto* map = new Map;
	auto raster = std::make_unique<FrameContextRasterTemplate>(map);
	auto* raster_ptr = raster.get();
	map->addTemplate(0, std::move(raster));
	TestMapEditor editor(map);
	editor.map_widget->resize(320, 240);
	auto* view = editor.map_widget->getMapView();
	view->setTemplateVisibility(raster_ptr, { 1, true });
	QTRY_VERIFY_WITH_TIMEOUT(raster_ptr->collection_count > 0, 1000);
	auto const resting_collections = raster_ptr->collection_count;

	auto const position = QPointF(160, 120);
	QWheelEvent wheel(
		position,
		QPointF(editor.map_widget->mapToGlobal(position.toPoint())),
		{}, { 0, 120 }, Qt::NoButton, Qt::NoModifier,
		Qt::ScrollUpdate, false);
	QApplication::sendEvent(editor.map_widget, &wheel);
	map->setTemplateAreaDirty(raster_ptr, QRectF(-10, -10, 20, 20), 0);
	QTest::qWait(60);
	QCOMPARE(raster_ptr->collection_count, resting_collections);
	QTRY_VERIFY_WITH_TIMEOUT(
		raster_ptr->collection_count > resting_collections, 1000);
}

void ToolsTest::sketchToolKeepsPrivateHistory()
{
	auto* map = new Map;
	TestMapEditor editor(map);
	editor.map_widget->resize(320, 240);
	SketchLayer::ensure(*map);
	auto* sketch_action =
	    actionWithText(editor.window, QStringLiteral("Sketch"));
	QVERIFY(sketch_action);
	QVERIFY(sketch_action->isEnabled());
	acceptFirstSketchLayer();
	sketch_action->trigger();
	auto* sketch_tool =
	    qobject_cast<SketchTool*>(editor.editor->getTool());
	QVERIFY(sketch_tool);
	QVERIFY(actionWithText(editor.window, QStringLiteral("Sketch layer")));
	QVERIFY(actionWithText(editor.window, QStringLiteral("Erase stroke")));
	QVERIFY(actionWithText(editor.window, QStringLiteral("Stroke width")));
	auto* sketch_undo =
	    actionWithText(editor.window, QStringLiteral("Undo sketch"));
	auto* sketch_redo =
	    actionWithText(editor.window, QStringLiteral("Redo sketch"));
	QVERIFY(sketch_undo);
	QVERIFY(sketch_redo);
	QVERIFY(!sketch_undo->isEnabled());
	QVERIFY(!sketch_redo->isEnabled());
	int committed_edits = 0;
	connect(map, &Map::editCommitted, map,
	        [&committed_edits] { ++committed_edits; });

	// A second finger/pinch cancels the provisional stroke. Its following
	// synthetic mouse release must not commit anything.
	QTest::mousePress(editor.map_widget, Qt::LeftButton, {}, QPoint(80, 80));
	QMouseEvent gesture_move(
	    QEvent::MouseMove, QPointF(150, 80),
	    QPointF(editor.map_widget->mapToGlobal(QPoint(150, 80))),
	    Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(editor.map_widget, &gesture_move);
	sketch_tool->gestureStarted();
	QTest::mouseRelease(
	    editor.map_widget, Qt::LeftButton, {}, QPoint(150, 80));
	QCOMPARE(SketchLayer::find(*map)->getNumObjects(), 0);
	QCOMPARE(committed_edits, 0);
	QVERIFY(!sketch_undo->isEnabled());

	editor.simulateDrag(QPoint(80, 120), QPoint(240, 120));
	auto* layer = SketchLayer::find(*map);
	QVERIFY(layer);
	QCOMPARE(layer->getNumObjects(), 1);
	QVERIFY(!map->undoManager().canUndo());
	QVERIFY(sketch_undo->isEnabled());
	QVERIFY(!sketch_redo->isEnabled());
	QCOMPARE(committed_edits, 1);

	// Ordinary map edits have a separate history and do not obscure the
	// selected sketch layer's next undo operation.
	map->undoManager().push(
	    std::make_unique<NoOpUndoStep>(map, true));
	QCOMPARE(committed_edits, 2);
	QVERIFY(map->undoManager().canUndo());
	QVERIFY(sketch_undo->isEnabled());
	sketch_undo->trigger();
	QCOMPARE(layer->getNumObjects(), 0);
	QCOMPARE(map->undoManager().nextUndoStep()->getType(),
	         UndoStep::ValidNoOpUndoStepType);
	QVERIFY(!sketch_undo->isEnabled());
	QVERIFY(sketch_redo->isEnabled());
	QCOMPARE(committed_edits, 3);
	sketch_redo->trigger();
	QCOMPARE(layer->getNumObjects(), 1);
	QVERIFY(sketch_undo->isEnabled());
	QVERIFY(!sketch_redo->isEnabled());
	QCOMPARE(committed_edits, 4);

	QAction* global_undo = nullptr;
	for (auto* action : editor.window->findChildren<QAction*>(
	         QString{}, Qt::FindDirectChildrenOnly))
	{
		if (action->text() == QStringLiteral("Undo"))
		{
			global_undo = action;
			break;
		}
	}
	QVERIFY(global_undo);
	global_undo->trigger();
	QCOMPARE(layer->getNumObjects(), 1);
	QVERIFY(!map->undoManager().canUndo());

	// A tool may be destroyed asynchronously after its controls have closed.
	// The layer's private history survives when Sketch is selected again.
	sketch_tool->deleteLater();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QVERIFY(!editor.editor->getTool());
	QVERIFY(!sketch_action->isChecked());
	sketch_action->trigger();
	QVERIFY(qobject_cast<SketchTool*>(editor.editor->getTool()));
	sketch_undo =
	    actionWithText(editor.window, QStringLiteral("Undo sketch"));
	sketch_redo =
	    actionWithText(editor.window, QStringLiteral("Redo sketch"));
	QVERIFY(sketch_undo);
	QVERIFY(sketch_redo);
	QVERIFY(sketch_undo->isEnabled());
	sketch_undo->trigger();
	QCOMPARE(layer->getNumObjects(), 0);
	QVERIFY(sketch_redo->isEnabled());
	sketch_redo->trigger();
	QCOMPARE(layer->getNumObjects(), 1);
}

void ToolsTest::sketchToolAnnotatesReadOnlyMap()
{
	auto* map = new Map;
	TestMapEditor editor(map);
	auto* layer = SketchLayer::ensure(*map);
	editor.editor->setReadOnly(true);
	auto* sketch_action =
	    actionWithText(editor.window, QStringLiteral("Sketch"));
	QVERIFY(sketch_action);
	QVERIFY(sketch_action->isEnabled());
	acceptFirstSketchLayer();
	sketch_action->trigger();
	QVERIFY(qobject_cast<SketchTool*>(editor.editor->getTool()));
	editor.simulateDrag(QPoint(80, 120), QPoint(240, 120));
	QCOMPARE(map->getNumParts(), 2);
	QCOMPARE(layer->getNumObjects(), 1);
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
