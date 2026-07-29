/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "tools/sketch_tool.h"

#include <algorithm>
#include <memory>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QBrush>
#include <QCoreApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QSettings>
#include <QSet>

#include "settings.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/map_view.h"
#include "core/objects/object.h"
#include "core/sketch_layer.h"
#include "core/symbols/sketch_symbol.h"
#include "gui/action_icon.h"
#include "gui/main_window.h"
#include "gui/map/map_editor.h"
#include "gui/map/map_widget.h"
#include "gui/util_gui.h"
#include "gui/widgets/action_grid_bar.h"
#include "undo/object_undo.h"
#include "undo/undo_manager.h"
#include "util/util.h"

namespace OpenOrienteering {

namespace {

bool isDark(const QColor& color) noexcept
{
	return qGray(color.rgb()) < 110;
}

void drawCheckmark(QPixmap& pixmap, const QColor& background)
{
	const auto size = pixmap.width();
	QPen pen(isDark(background) ? Qt::white : Qt::black);
	pen.setWidth(std::max(2, size / 9));
	pen.setCapStyle(Qt::RoundCap);
	QPainter painter(&pixmap);
	painter.setPen(pen);
	painter.drawLine(6 * size / 20, 11 * size / 20,
	                 8 * size / 20, 13 * size / 20);
	painter.drawLine(8 * size / 20, 13 * size / 20,
	                 14 * size / 20, 6 * size / 20);
}

QIcon makeEraserIcon(int size)
{
	QPixmap pixmap(size, size);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(QApplication::palette().color(QPalette::Text),
	                    std::max(1, size / 18)));
	painter.setBrush(QColor(170, 170, 175));
	const QPolygonF eraser{
	    QPointF(0.22 * size, 0.70 * size),
	    QPointF(0.55 * size, 0.18 * size),
	    QPointF(0.82 * size, 0.42 * size),
	    QPointF(0.48 * size, 0.78 * size)};
	painter.drawPolygon(eraser);
	painter.drawLine(QPointF(0.18 * size, 0.82 * size),
	                 QPointF(0.84 * size, 0.82 * size));
	return QIcon(pixmap);
}

QIcon makeWidthIcon(int size, SketchLayer::Width width)
{
	QPixmap pixmap(size, size);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	QPen pen(QApplication::palette().color(QPalette::Text));
	switch (width)
	{
	case SketchLayer::Width::Fine: pen.setWidthF(std::max(1.5, size * 0.08)); break;
	case SketchLayer::Width::Medium: pen.setWidthF(std::max(2.5, size * 0.15)); break;
	case SketchLayer::Width::Broad: pen.setWidthF(std::max(4.0, size * 0.25)); break;
	}
	pen.setCapStyle(Qt::RoundCap);
	painter.setPen(pen);
	painter.drawLine(QPointF(0.18 * size, 0.50 * size),
	                 QPointF(0.82 * size, 0.50 * size));
	return QIcon(pixmap);
}

QString translatedWidthName(SketchLayer::Width width)
{
	switch (width)
	{
	case SketchLayer::Width::Fine:
		return QCoreApplication::translate("SketchTool", "Fine");
	case SketchLayer::Width::Broad:
		return QCoreApplication::translate("SketchTool", "Broad");
	case SketchLayer::Width::Medium:
		return QCoreApplication::translate("SketchTool", "Medium");
	}
	return QCoreApplication::translate("SketchTool", "Medium");
}

}  // namespace

SketchTool::SketchTool(MapEditorController* editor, QAction* tool_action)
 : MapEditorTool(editor, Scribble, tool_action)
{
	connect(map(), &Map::mapPartDeleted, this,
	        [this](std::size_t /*index*/, const MapPart* deleted) {
		        if (deleted == layer)
		        {
			        layer = nullptr;
			        deactivate();
		        }
	        });
}

SketchTool::~SketchTool()
{
	if (widget)
		editor->deletePopupWidget(widget);
	undo_action = nullptr;
	redo_action = nullptr;
}

void SketchTool::setLayer(MapPart* new_layer, UndoManager* new_history)
{
	if (history)
		history->disconnect(this);
	layer = new_layer;
	history = new_history;
	if (history)
	{
		auto update_actions = [this] {
			updateUndoRedoAvailability();
		};
		connect(history, &UndoManager::canUndoChanged,
		        this, update_actions);
		connect(history, &UndoManager::canRedoChanged,
		        this, update_actions);
	}
	updateUndoRedoAvailability();
}

void SketchTool::setChooseLayerCallback(std::function<void()> callback)
{
	choose_layer_callback = std::move(callback);
}

void SketchTool::init()
{
	setStatusBarText(
	    tr("<b>Drag</b>: Sketch. <b>Right drag</b>: Erase touched strokes."));
	widget = makeToolBar();
	editor->showPopupWidget(widget, tr("Sketch"));
	MapEditorTool::init();
}

const QCursor& SketchTool::getCursor() const
{
	static const auto cursor = scaledToScreen(
	    QCursor{QPixmap(QStringLiteral(":/images/cursor-paint-on-template.png")),
	            1, 1});
	return cursor;
}

ActionGridBar* SketchTool::makeToolBar()
{
	QSettings settings;
	const auto selected =
	    settings.value(QStringLiteral("PaintOnTemplateTool/selectedColor"))
	        .toString();
	stroke_width = SketchLayer::widthFromSetting(
	    settings.value(QStringLiteral("SketchTool/width"), 1).toInt());

	const auto icon_size = Util::mmToPixelPhysical(
	    Settings::getInstance()
	        .getSetting(Settings::ActionGridBar_ButtonSizeMM)
	        .toReal());
	auto* toolbar = new ActionGridBar(ActionGridBar::Horizontal, 2);
	auto* modes = new QActionGroup(this);
	modes->setExclusive(true);

	auto* layer_action = new QAction(
	    ActionIcon::fromName(u"map-parts"), tr("Sketch layer"), toolbar);
	connect(layer_action, &QAction::triggered, this, [this] {
		if (choose_layer_callback)
			choose_layer_callback();
	});
	toolbar->addAction(layer_action, 0, 0);

	auto colors = Settings::getInstance().paintOnTemplateColors();
	if (colors.empty())
		colors.push_back(Qt::black);
	auto count = std::size_t{2};
	for (const auto& color : colors)
	{
		QPixmap pixmap(icon_size, icon_size);
		pixmap.fill(color);
		QIcon icon(pixmap);
		drawCheckmark(pixmap, color);
		icon.addPixmap(pixmap, QIcon::Normal, QIcon::On);
		auto* action =
		    new QAction(icon, color.name(QColor::HexArgb), toolbar);
		action->setCheckable(true);
		action->setActionGroup(modes);
		toolbar->addAction(action, count % 2, count / 2);
		if (count == 0 || action->text() == selected)
		{
			stroke_color = color;
			action->setChecked(true);
		}
		connect(action, &QAction::triggered, this,
		        [this, color] { setColor(color); });
		++count;
	}

	auto* erase_action =
	    new QAction(makeEraserIcon(icon_size), tr("Erase stroke"), toolbar);
	erase_action->setCheckable(true);
	erase_action->setActionGroup(modes);
	connect(erase_action, &QAction::triggered, this,
	        [this] { explicit_erasing = true; });
	toolbar->addAction(erase_action, count % 2, count / 2);

	width_action = new QAction(
	    makeWidthIcon(icon_size, stroke_width), tr("Stroke width"), toolbar);
	auto* width_menu = new QMenu(toolbar);
	auto* widths = new QActionGroup(width_menu);
	widths->setExclusive(true);
	for (const auto width : {SketchLayer::Width::Fine,
	                         SketchLayer::Width::Medium,
	                         SketchLayer::Width::Broad})
	{
		auto* action = width_menu->addAction(
		    makeWidthIcon(icon_size, width),
		    translatedWidthName(width));
		action->setCheckable(true);
		action->setChecked(width == stroke_width);
		action->setActionGroup(widths);
		connect(action, &QAction::triggered, this,
		        [this, width] { setWidth(width); });
	}
	width_action->setMenu(width_menu);
	toolbar->addActionAtEnd(width_action, 0, 1);

	undo_action =
	    new QAction(ActionIcon::fromName(u"undo"), tr("Undo sketch"), toolbar);
	connect(undo_action, &QAction::triggered,
	        this, &SketchTool::undoSketch);
	toolbar->addActionAtEnd(undo_action, 0, 0);

	redo_action =
	    new QAction(ActionIcon::fromName(u"redo"), tr("Redo sketch"), toolbar);
	connect(redo_action, &QAction::triggered,
	        this, &SketchTool::redoSketch);
	toolbar->addActionAtEnd(redo_action, 1, 0);
	updateUndoRedoAvailability();
	return toolbar;
}

void SketchTool::updateUndoRedoAvailability()
{
	if (!undo_action || !redo_action)
		return;
	undo_action->setEnabled(history && history->canUndo());
	redo_action->setEnabled(history && history->canRedo());
}

void SketchTool::undoSketch()
{
	if (history && history->canUndo())
		history->undo(mainWindow());
}

void SketchTool::redoSketch()
{
	if (history && history->canRedo())
		history->redo(mainWindow());
}

void SketchTool::setColor(const QColor& color)
{
	explicit_erasing = false;
	stroke_color = color;
	QSettings().setValue(
	    QStringLiteral("PaintOnTemplateTool/selectedColor"),
	    color.name(QColor::HexArgb));
}

void SketchTool::setWidth(SketchLayer::Width width)
{
	stroke_width = width;
	QSettings().setValue(
	    QStringLiteral("SketchTool/width"),
	    SketchLayer::widthSetting(width));
	if (width_action)
	{
		const auto icon_size = Util::mmToPixelPhysical(
		    Settings::getInstance()
		        .getSetting(Settings::ActionGridBar_ButtonSizeMM)
		        .toReal());
		width_action->setIcon(makeWidthIcon(icon_size, width));
		width_action->setToolTip(
		    tr("%1 sketch stroke").arg(translatedWidthName(width)));
	}
}

bool SketchTool::mousePressEvent(
        QMouseEvent* event, const MapCoordF& map_coord, MapWidget* /*widget*/)
{
	if (dragging || !layer)
		return true;
	if (event->button() != Qt::LeftButton
	    && event->button() != Qt::RightButton)
		return false;
	coords.clear();
	coords.push_back(map_coord);
	map_bbox = QRectF(map_coord.x(), map_coord.y(), 0, 0);
	last_sample_pos = event->pos();
	stroke_erasing =
	    explicit_erasing || event->button() == Qt::RightButton;
	dragging = true;
	return true;
}

bool SketchTool::mouseMoveEvent(
        QMouseEvent* event, const MapCoordF& map_coord, MapWidget* map_widget)
{
	if (!dragging || !layer)
		return false;
	if ((last_sample_pos - event->pos()).manhattanLength() <= 2)
		return true;
	coords.push_back(map_coord);
	rectInclude(map_bbox, map_coord);
	last_sample_pos = event->pos();
	updateDrawingBounds(map_widget);
	return true;
}

bool SketchTool::mouseReleaseEvent(
        QMouseEvent* /*event*/, const MapCoordF& map_coord,
        MapWidget* map_widget)
{
	if (!dragging || !layer)
		return false;
	coords.push_back(map_coord);
	rectInclude(map_bbox, map_coord);
	finishStroke(map_widget);
	coords.clear();
	map()->clearDrawingBoundingBox();
	dragging = false;
	return true;
}

bool SketchTool::keyPressEvent(QKeyEvent* event)
{
	if (event->key() == Qt::Key_Escape)
	{
		coords.clear();
		map()->clearDrawingBoundingBox();
		dragging = false;
		return true;
	}
	return false;
}

void SketchTool::gestureStarted()
{
	if (!dragging)
		return;
	coords.clear();
	map()->clearDrawingBoundingBox();
	dragging = false;
}

void SketchTool::finishStroke(MapWidget* map_widget)
{
	if (stroke_erasing)
	{
		eraseTouchedStrokes();
		return;
	}
	if (!layer || coords.empty())
		return;
	const auto tolerance =
	    0.001 * map_widget->getMapView()->pixelToLength(0.65);
	auto simplified = SketchLayer::simplify(coords, tolerance);
	if (simplified.empty())
		return;
	if (simplified.size() == 1
	    || simplified.front().distanceSquaredTo(simplified.back()) < 1e-8)
	{
		simplified.push_back(
		    simplified.front() + MapCoordF(0.001, 0));
	}
	MapCoordVector path_coords;
	path_coords.reserve(simplified.size());
	std::transform(
	    simplified.cbegin(), simplified.cend(),
	    std::back_inserter(path_coords),
	    [](const MapCoordF& coord) { return MapCoord(coord); });

	const auto part_index = map()->findPartIndex(layer);
	auto* symbol =
	    SketchLayer::ensureSymbol(*map(), stroke_color, stroke_width);
	auto* object =
	    new PathObject(symbol, std::move(path_coords), map());
	const auto object_index = map()->addObject(object, part_index);
	auto* undo_step = new DeleteObjectsUndoStep(map());
	undo_step->setPartIndex(part_index);
	undo_step->addObject(object_index);
	map()->setObjectsDirty();
	history->push(std::unique_ptr<UndoStep>(undo_step));
	updateUndoRedoAvailability();
}

void SketchTool::eraseTouchedStrokes()
{
	if (!layer || coords.empty())
		return;
	QSet<Object*> touched;
	for (int index = 0; index < layer->getNumObjects(); ++index)
	{
		auto* object = layer->getObject(index);
		auto* path = dynamic_cast<PathObject*>(object);
		auto* symbol = object
		                   ? dynamic_cast<const SketchSymbol*>(
		                         object->getSymbol())
		                   : nullptr;
		if (path && symbol)
		{
			const auto tolerance =
			    eraser_radius_mm + 0.0005 * symbol->getLineWidth();
			if (SketchLayer::intersectsStroke(coords, *path, tolerance))
				touched.insert(object);
		}
	}
	if (touched.isEmpty())
		return;

	const auto part_index = map()->findPartIndex(layer);
	auto* undo_step = new AddObjectsUndoStep(map());
	undo_step->setPartIndex(part_index);
	std::vector<std::pair<int, Object*>> ordered;
	ordered.reserve(touched.size());
	for (auto* object : touched)
		ordered.emplace_back(layer->findObjectIndex(object), object);
	std::sort(ordered.begin(), ordered.end());
	for (const auto& [index, object] : ordered)
		undo_step->addObject(index, object);
	undo_step->removeContainedObjects(false);
	map()->setObjectsDirty();
	history->push(std::unique_ptr<UndoStep>(undo_step));
	updateUndoRedoAvailability();
}

void SketchTool::updateDrawingBounds(MapWidget* map_widget)
{
	const auto width_mm =
	    stroke_erasing ? 2 * eraser_radius_mm
	                   : SketchLayer::widthMillimeters(stroke_width);
	const auto border = map_widget->getMapView()->lengthToPixel(
	    1000 * width_mm);
	map()->setDrawingBoundingBox(map_bbox, qCeil(border));
}

void SketchTool::draw(
        render::OverlaySceneBuilder* painter, MapWidget* map_widget)
{
	if (!dragging || coords.empty())
		return;
	QPen pen(stroke_erasing ? QColor(255, 255, 255, 190)
	                       : stroke_color);
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	const auto width_mm =
	    stroke_erasing ? 2 * eraser_radius_mm
	                   : SketchLayer::widthMillimeters(stroke_width);
	pen.setWidthF(std::max(
	    qreal(1.25),
	    map_widget->getMapView()->lengthToPixel(1000 * width_mm)));
	QPolygonF polygon;
	polygon.reserve(coords.size());
	for (const auto& coord : coords)
		polygon.append(map_widget->mapToViewport(coord));
	painter->setPen(pen);
	painter->setBrush(Qt::NoBrush);
	painter->drawPolyline(polygon);
}

}  // namespace OpenOrienteering
