/*
 *    Copyright 2012-2014 Thomas Schöps
 *    Copyright 2013-2020 Kai Pastor
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


#include "map_widget.h"

#include <cmath>
#include <shared_mutex>
#include <stdexcept>

#include <QApplication>
#include <QColor>
#include <QContextMenuEvent>
#include <QEvent>
#include <QFlags>
#include <QFont>
#include <QGestureEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLatin1String>
#include <QList>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QObjectList>
#include <QPainter>
#include <QPaintEvent>
#include <QPinchGesture>
#include <QPointer>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QToolTip>
#include <QTouchEvent>
#include <QTransform>
#include <QVariant>
#include <QWheelEvent>

#include "settings.h"
#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map.h"
#include "core/renderables/renderable.h"
#include "core/symbols/symbol.h"  // IWYU pragma: keep
#include "gui/touch_cursor.h"
#include "gui/map/map_editor_activity.h"
#include "gui/widgets/action_grid_bar.h"
#include "gui/widgets/key_button_bar.h"
#include "gui/widgets/pie_menu.h"
#include "sensors/gps_display.h"
#include "sensors/gps_temporary_markers.h"
#include "templates/template.h"
#include "tools/tool.h"
#include "util/backports.h" // IWYU pragma: keep
#include "util/util.h"

class QGesture;
// IWYU pragma: no_forward_declare QPinchGesture


#ifdef __clang_analyzer__
#define singleShot(A, B, C) singleShot(A, B, #C) // NOLINT 
#endif


namespace OpenOrienteering {

MapWidget::MapWidget(bool show_help, bool force_antialiasing, QWidget* parent)
 : QWidget(parent)
 , view(nullptr)
 , tool(nullptr)
 , activity(nullptr)
 , coords_type(MAP_COORDS)
 , cursorpos_label(nullptr)
 , show_help(show_help)
 , force_antialiasing(force_antialiasing)
 , dragging(false)
 , pinching(false)
 , pinching_factor(1.0)
 , render_context_update_scheduled(false)
 , scene_dirty_rect(rect())
 , drawing_dirty_rect_border(0)
 , activity_dirty_rect_border(0)
 , last_mouse_release_time(QTime::currentTime())
 , current_pressed_buttons(0)
 , gps_display(nullptr)
 , marker_display(nullptr)
{
	context_menu = new PieMenu(this);
// 	context_menu->setMinimumActionCount(8);
// 	context_menu->setIconSize(24);
	
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_AcceptTouchEvents, true);
	setGesturesEnabled(true);
	setAutoFillBackground(false);
	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);
	setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));

	tile_scheduler = new TileRenderScheduler(this);
	connect(tile_scheduler, &TileRenderScheduler::resultsReady,
	        this, &MapWidget::installSceneTileResults);
}

MapWidget::~MapWidget()
{
	// nothing, not inlined
}

void MapWidget::setMapView(MapView* view)
{
	if (this->view != view)
	{
		if (this->view)
		{
			auto* map = this->view->getMap();
			map->removeMapWidget(this);
			disconnect(map);
			disconnect(this->view);
			for (int i = 0; i < map->getNumTemplates(); ++i)
				disconnect(map->getTemplate(i), nullptr, this, nullptr);
		}
		
		this->view = view;
		
		if (view)
		{
			connect(this->view, &MapView::viewChanged, this, &MapWidget::viewChanged);
			connect(this->view, &MapView::visibilityChanged, this, &MapWidget::visibilityChanged);
			connect(this->view, &MapView::panOffsetChanged, this, &MapWidget::setPanOffset);
			
			auto* map = this->view->getMap();
			map->addMapWidget(this);
			connect(map, &Map::colorAdded, this, &MapWidget::updatePlaceholder);
			connect(map, &Map::colorDeleted, this, &MapWidget::updatePlaceholder);
			connect(map, &Map::symbolAdded, this, &MapWidget::updatePlaceholder);
			connect(map, &Map::symbolDeleted, this, &MapWidget::updatePlaceholder);
			connect(map, &Map::templateAdded, this, &MapWidget::updatePlaceholder);
			connect(map, &Map::templateDeleted, this, &MapWidget::updatePlaceholder);
			for (int i = 0; i < map->getNumTemplates(); ++i)
				connect(map->getTemplate(i), &Template::templateStateChanged, this, &MapWidget::scheduleRenderContextUpdate);
			connect(map, &Map::templateAdded, this, [this](int, Template* temp) {
				connect(temp, &Template::templateStateChanged, this, &MapWidget::scheduleRenderContextUpdate);
				scheduleRenderContextUpdate();
			});
			connect(map, &Map::templateChanged, this, &MapWidget::scheduleRenderContextUpdate);

			scheduleRenderContextUpdate();
		}
		
		update();
	}
}

void MapWidget::setTool(MapEditorTool* tool)
{
	// Redraw if touch cursor usage changes
	bool redrawTouchCursor = (touch_cursor && this->tool && tool
		&& (this->tool->usesTouchCursor() || tool->usesTouchCursor()));

	this->tool = tool;
	
	if (tool)
		setCursor(tool->getCursor());
	else
		unsetCursor();
	if (redrawTouchCursor)
		touch_cursor->updateMapWidget(false);
}

void MapWidget::suspendTileRendering()
{
	if (tile_scheduler)
		tile_scheduler->suspend();
}

void MapWidget::resumeTileRendering()
{
	if (tile_scheduler)
		tile_scheduler->resume();
}

void MapWidget::setActivity(MapEditorActivity* activity)
{
	this->activity = activity;
}


void MapWidget::setGesturesEnabled(bool enabled)
{
	gestures_enabled = enabled;
	if (enabled)
	{
		grabGesture(Qt::PinchGesture);
	}
	else
	{
		ungrabGesture(Qt::PinchGesture);
	}
}


void MapWidget::applyMapTransform(QPainter* painter) const
{
	painter->translate(width() / 2.0 + getMapView()->panOffset().x(),
					   height() / 2.0 + getMapView()->panOffset().y());
	painter->setWorldTransform(getMapView()->worldTransform(), true);
}

QRectF MapWidget::viewportToView(const QRect& input) const
{
	return QRectF(input.left() - 0.5*width() - pan_offset.x(), input.top() - 0.5*height() - pan_offset.y(), input.width(), input.height());
}

QPointF MapWidget::viewportToView(const QPoint& input) const
{
	return QPointF(input.x() - 0.5*width() - pan_offset.x(), input.y() - 0.5*height() - pan_offset.y());
}

QPointF MapWidget::viewportToView(QPointF input) const
{
	input.rx() -= 0.5*width() + pan_offset.x();
	input.ry() -= 0.5*height() + pan_offset.y();
	return input;
}

QRectF MapWidget::viewToViewport(const QRectF& input) const
{
	return QRectF(input.left() + 0.5*width() + pan_offset.x(), input.top() + 0.5*height() + pan_offset.y(), input.width(), input.height());
}

QRectF MapWidget::viewToViewport(const QRect& input) const
{
	return QRectF(input.left() + 0.5*width() + pan_offset.x(), input.top() + 0.5*height() + pan_offset.y(), input.width(), input.height());
}

QPointF MapWidget::viewToViewport(const QPoint& input) const
{
	return QPointF(input.x() + 0.5*width() + pan_offset.x(), input.y() + 0.5*height() + pan_offset.y());
}

QPointF MapWidget::viewToViewport(QPointF input) const
{
	input.rx() += 0.5*width() + pan_offset.x();
	input.ry() += 0.5*height() + pan_offset.y();
	return input;
}


MapCoord MapWidget::viewportToMap(const QPoint& input) const
{
	return view->viewToMap(viewportToView(input));
}

MapCoordF MapWidget::viewportToMapF(const QPoint& input) const
{
	return view->viewToMapF(viewportToView(input));
}

MapCoordF MapWidget::viewportToMapF(const QPointF& input) const
{
	return view->viewToMapF(viewportToView(input));
}

QPointF MapWidget::mapToViewport(const MapCoord& input) const
{
	return viewToViewport(view->mapToView(input));
}

QPointF MapWidget::mapToViewport(const QPointF& input) const
{
	return viewToViewport(view->mapToView(input));
}

QRectF MapWidget::mapToViewport(const QRectF& input) const
{
	QRectF result;
	rectIncludeSafe(result, mapToViewport(input.topLeft()));
	rectIncludeSafe(result, mapToViewport(input.bottomRight()));
	if (view->getRotation() != 0)
	{
		rectIncludeSafe(result, mapToViewport(input.topRight()));
		rectIncludeSafe(result, mapToViewport(input.bottomLeft()));
	}
	return result;
}

void MapWidget::scheduleRenderContextUpdate()
{
	if (!view || render_context_update_scheduled)
		return;

	render_context_update_scheduled = true;
	QTimer::singleShot(0, this, &MapWidget::publishRenderContext);
}

void MapWidget::publishRenderContext()
{
	render_context_update_scheduled = false;
	if (!view || view->areAllTemplatesHidden())
		return;

	auto context = currentViewRenderContext();

	auto* map = view->getMap();
	for (int i = 0; i < map->getNumTemplates(); ++i)
	{
		auto* temp = map->getTemplate(i);
		if (temp->getTemplateState() != Template::Loaded || !view->isTemplateVisible(temp))
			continue;

		temp->updateRenderContext(context);
	}
}

ViewRenderContext MapWidget::currentViewRenderContext() const
{
	Q_ASSERT(view);

	ViewRenderContext context;
	context.visible_map_rect = view->calculateViewedRect(viewportToView(rect()));
	context.view_zoom = view->getZoom();
	return context;
}

void MapWidget::viewChanged(MapView::ChangeFlags changes)
{
	setDrawingBoundingBox(drawing_dirty_rect_map, drawing_dirty_rect_border, true);
	setActivityBoundingBox(activity_dirty_rect_map, activity_dirty_rect_border, true);

	if (changes & (MapView::ZoomChange | MapView::RotationChange))
	{
		tile_scheduler->cancelPending();
		scene_store.resetInFlight();
		if (!has_zoom_fallback)
		{
			fallback_transform = last_rendered_transform;
			scene_store.promoteToFallback();
			has_zoom_fallback = true;
		}
		else
		{
			scene_store.clear();
		}
		pan_adjusted = false;
		updateEverything();
	}
	else if (pan_adjusted)
	{
		// Grid offsets were already adjusted in finishDragging — tiles
		// remain valid at their new positions.  Just repaint; new edge
		// tiles will be dispatched by updateSceneTiles().
		pan_adjusted = false;
		update();
	}
	else
	{
		tile_scheduler->cancelPending();
		scene_store.resetInFlight();
		scene_store.clear();
		updateEverything();
	}
	scheduleRenderContextUpdate();
	if (changes.testFlag(MapView::ZoomChange))
		updateZoomDisplay();
}

void MapWidget::visibilityChanged(MapView::VisibilityFeature feature, bool active, Template* temp)
{
	switch (feature)
	{
	case MapView::VisibilityFeature::TemplateVisible:
		if (temp && temp->getTemplateState() == Template::Loaded)
		{
			auto const pos = getMapView()->getMap()->findTemplateIndex(temp);
			auto const template_area = temp->calculateTemplateBoundingBox();
			markTemplateCacheDirty(getMapView()->calculateViewBoundingBox(template_area),
			                       temp->getTemplateBoundingBoxPixelBorder(),
			                       pos >= getMapView()->getMap()->getFirstFrontTemplate());
		
		}
		else if (temp && temp->getTemplateState() == Template::Unloaded && active)
		{
			// The template must be loaded.
			QToolTip::showText(QCursor::pos(),
			                   qApp->translate("OpenOrienteering::MainWindow", "Opening %1")
			                   .arg(temp->getTemplateFilename()) );
			// Use a small delay so that some UI events can be processed first.
			QPointer<MapWidget> widget(this);
			QTimer::singleShot(10, temp, ([temp, widget]() {
				if (temp->getTemplateState() != Template::Loaded)
				{
					temp->loadTemplateFile();
					QToolTip::hideText();
					if (temp->getTemplateState() == Template::Invalid)
						QMessageBox::warning(widget.data(),
						                     qApp->translate("OpenOrienteering::MainWindow", "Error"),
						                     qApp->translate("OpenOrienteering::Importer", "Failed to load template '%1', reason: %2")
						                     .arg(temp->getTemplateFilename(), temp->errorString()) );
					else if (widget)
						widget->scheduleRenderContextUpdate();
				}
			}));
		}
		break;
		
	case MapView::VisibilityFeature::GridVisible:
	case MapView::VisibilityFeature::MapVisible:
		scene_dirty_rect = rect();
		scene_store.dirtyAll();
		Q_FALLTHROUGH();
	case MapView::VisibilityFeature::AllTemplatesHidden:
		update();
		break;
		
	default:
		updateEverything();
		break;
	}

	scheduleRenderContextUpdate();
}

void MapWidget::setPanOffset(const QPoint& offset)
{
	pan_offset = offset;
	scheduleRenderContextUpdate();
	update();
}

void MapWidget::startDragging(const QPoint& cursor_pos)
{
	Q_ASSERT(!dragging);
	Q_ASSERT(!pinching);
	dragging = true;
	drag_start_pos = cursor_pos;
	normal_cursor  = cursor();
	setCursor(Qt::ClosedHandCursor);
}

void MapWidget::updateDragging(const QPoint& cursor_pos)
{
	Q_ASSERT(dragging);
	view->setPanOffset(cursor_pos - drag_start_pos);
}

void MapWidget::finishDragging(const QPoint& cursor_pos)
{
	Q_ASSERT(dragging);
	dragging = false;

	QPoint pan_delta = cursor_pos - drag_start_pos;

	// Adjust grid offsets to compensate for the center change that
	// finishPanning will make.  After the center shifts, old view point v
	// maps to v + pan_delta in the new view space, so shift the grid by
	// +pan_delta to keep tiles aligned.
	adjustAllGridOffsets(QPointF(pan_delta.x(), pan_delta.y()));
	pan_adjusted = true;

	view->finishPanning(pan_delta);
	setCursor(normal_cursor);
}

void MapWidget::cancelDragging()
{
	dragging = false;
	view->setPanOffset(QPoint());
	setCursor(normal_cursor);
}

qreal MapWidget::startPinching(const QPoint& center)
{
	Q_ASSERT(!dragging);
	Q_ASSERT(!pinching);
	pinching = true;
	drag_start_pos  = center;
	pinching_center = center;
	pinching_factor = 1.0;
	return pinching_factor;
}

void MapWidget::updatePinching(const QPoint& center, qreal factor)
{
	Q_ASSERT(pinching);
	pinching_center = center;
	pinching_factor = factor;
	updateZoomDisplay();
	update();
}

void MapWidget::finishPinching(const QPoint& center, qreal factor)
{
	pinching = false;
	view->finishPanning(center - drag_start_pos);
	view->setZoom(factor * view->getZoom(), viewportToView(center));
}

void MapWidget::cancelPinching()
{
	pinching = false;
	pinching_factor = 1.0;
	update();
}

void MapWidget::moveMap(int steps_x, int steps_y)
{
	if (steps_x != 0 || steps_y != 0)
	{
		try
		{
			constexpr auto move_factor = 0.25;
			auto offset = MapCoord::fromNative64( qRound64(view->pixelToLength(width() * steps_x * move_factor)),
			                                      qRound64(view->pixelToLength(height() * steps_y * move_factor)) );
			view->setCenter(view->center() + offset);
		}
		catch (std::range_error&)
		{
			// Do nothing
		}
	}
}

void MapWidget::ensureVisibilityOfRect(QRectF map_rect, ZoomOption zoom_option)
{
	// Amount in pixels that is scrolled "too much" if the rect is not completely visible
	// TODO: change to absolute size using dpi value
	const int pixel_border = 70;
	auto viewport_rect = mapToViewport(map_rect).toAlignedRect();
	
	// TODO: this method assumes that the viewport is not rotated.
	
	if (rect().contains(viewport_rect.topLeft()) && rect().contains(viewport_rect.bottomRight()))
		return;
	
	auto offset = MapCoordF{ 0, 0 };
	
	if (viewport_rect.left() < 0)
		offset.rx() = view->pixelToLength(viewport_rect.left() - pixel_border) / 1000.0;
	else if (viewport_rect.right() > width())
		offset.rx() = view->pixelToLength(viewport_rect.right() - width() + pixel_border) / 1000.0;
	
	if (viewport_rect.top() < 0)
		offset.ry() = view->pixelToLength(viewport_rect.top() - pixel_border) / 1000.0;
	else if (viewport_rect.bottom() > height())
		offset.ry() = view->pixelToLength(viewport_rect.bottom() - height() + pixel_border) / 1000.0;
	
	if (!qIsNull(offset.lengthSquared()))
		view->setCenter(view->center() + offset);
	
	// If the rect is still not completely in view, we have to zoom out
	viewport_rect = mapToViewport(map_rect).toAlignedRect();
	if (!(rect().contains(viewport_rect.topLeft()) && rect().contains(viewport_rect.bottomRight())))
		adjustViewToRect(map_rect, zoom_option);
}

void MapWidget::adjustViewToRect(QRectF map_rect, ZoomOption zoom_option)
{
	view->setCenter(MapCoord{ map_rect.center() });
	
	if (map_rect.isValid())
	{
		// NOTE: The loop is an inelegant way to fight inaccuracies that occur somewhere ...
		const int pixel_border = 15;
		const float initial_zoom = view->getZoom();
		for (int i = 0; i < 10; ++i)
		{
			float zoom_factor = qMin(height() / (view->lengthToPixel(1000.0 * map_rect.height()) + 2*pixel_border),
			                         width() / (view->lengthToPixel(1000.0 * map_rect.width()) + 2*pixel_border));
			float zoom = view->getZoom() * zoom_factor;
			if (zoom_option == DiscreteZoom)
			{
				zoom = pow(2, 0.5 * floor(2.0 * (std::log2(zoom) - std::log2(initial_zoom))) + std::log2(initial_zoom));
			}
			view->setZoom(zoom);
		}
	}
}

void MapWidget::moveDirtyRect(QRect& dirty_rect, qreal x, qreal y)
{
	if (dirty_rect.isValid())
		dirty_rect = dirty_rect.translated(x, y).intersected(rect());
}

void MapWidget::markTemplateCacheDirty(const QRectF& view_rect, int pixel_border, bool /*front_cache*/)
{
	// All layers share one scene store — dirty the tiles that overlap.
	QRectF padded = view_rect.adjusted(-pixel_border, -pixel_border, pixel_border, pixel_border);
	scene_store.dirtyViewRect(padded);

	QRectF viewport_rect = viewToViewport(view_rect);
	QRect integer_rect = QRect(viewport_rect.left() - (1+pixel_border), viewport_rect.top() - (1+pixel_border),
	                           viewport_rect.width() + 2*(1+pixel_border), viewport_rect.height() + 2*(1+pixel_border));
	if (!integer_rect.intersects(rect()))
		return;
	if (scene_dirty_rect.isValid())
		scene_dirty_rect = scene_dirty_rect.united(integer_rect);
	else
		scene_dirty_rect = integer_rect;

	update(integer_rect);
}

void MapWidget::markObjectAreaDirty(const QRectF& map_rect)
{
	QRectF view_rect = view->calculateViewBoundingBox(map_rect);
	scene_store.dirtyViewRect(view_rect);
	updateMapRect(map_rect, 0, scene_dirty_rect);
}

void MapWidget::setDrawingBoundingBox(QRectF map_rect, int pixel_border, bool do_update)
{
	Q_UNUSED(do_update);
	clearDrawingBoundingBox();
	if (map_rect.isValid())
	{
		drawing_dirty_rect_map = map_rect;
		drawing_dirty_rect_border = pixel_border;
		updateMapRect(drawing_dirty_rect_map, drawing_dirty_rect_border, drawing_dirty_rect);
	}
}

void MapWidget::clearDrawingBoundingBox()
{
	drawing_dirty_rect_map.setWidth(0);
	if (drawing_dirty_rect.isValid())
	{
		update(drawing_dirty_rect);
		drawing_dirty_rect.setWidth(0);
	}
}

void MapWidget::setActivityBoundingBox(QRectF map_rect, int pixel_border, bool do_update)
{
	Q_UNUSED(do_update);
	clearActivityBoundingBox();
	if (map_rect.isValid())
	{
		activity_dirty_rect_map = map_rect;
		activity_dirty_rect_border = pixel_border;
		updateMapRect(activity_dirty_rect_map, activity_dirty_rect_border, activity_dirty_rect);
	}
}

void MapWidget::clearActivityBoundingBox()
{
	activity_dirty_rect_map.setWidth(0);
	if (activity_dirty_rect.isValid())
	{
		update(activity_dirty_rect);
		activity_dirty_rect.setWidth(0);
	}
}

void MapWidget::updateDrawing(const QRectF& map_rect, int pixel_border)
{
	QRect viewport_rect = calculateViewportBoundingBox(map_rect, pixel_border);
	
	if (viewport_rect.intersects(rect()))
		update(viewport_rect);
}

void MapWidget::updateMapRect(const QRectF& map_rect, int pixel_border, QRect& cache_dirty_rect)
{
	QRect viewport_rect = calculateViewportBoundingBox(map_rect, pixel_border);
	updateViewportRect(viewport_rect, cache_dirty_rect);
}

void MapWidget::updateViewportRect(QRect viewport_rect, QRect& cache_dirty_rect)
{
	if (viewport_rect.intersects(rect()))
	{
		if (cache_dirty_rect.isValid())
			cache_dirty_rect = cache_dirty_rect.united(viewport_rect);
		else
			cache_dirty_rect = viewport_rect;
		
		update(viewport_rect);
	}
}

void MapWidget::updateDrawingLater(const QRectF& map_rect, int pixel_border)
{
	QRect viewport_rect = calculateViewportBoundingBox(map_rect, pixel_border);
	
	if (viewport_rect.intersects(rect()))
	{
		if (!cached_update_rect.isValid())
		{
			// Start the update timer
			QTimer::singleShot(15, this, &MapWidget::updateDrawingLaterSlot);
		}
		
		// NOTE: this may require a mutex for concurrent access with updateDrawingLaterSlot()?
		rectIncludeSafe(cached_update_rect, viewport_rect);
	}
}

void MapWidget::updateDrawingLaterSlot()
{
	updateEverythingInRect(cached_update_rect);
	cached_update_rect = QRect();
}

void MapWidget::updateEverything()
{
	scene_dirty_rect = rect();
	scene_store.dirtyAll();
	update(scene_dirty_rect);
}

void MapWidget::updateEverythingInRect(const QRect& dirty_rect)
{
	QRectF dirty_view = viewportToView(dirty_rect);
	rectIncludeSafe(scene_dirty_rect, dirty_rect);
	scene_store.dirtyViewRect(dirty_view);
	update(dirty_rect);
}

QRect MapWidget::calculateViewportBoundingBox(const QRectF& map_rect, int pixel_border) const
{
	QRectF view_rect = view->calculateViewBoundingBox(map_rect);
	view_rect.adjust(-pixel_border, -pixel_border, +pixel_border, +pixel_border);
	return viewToViewport(view_rect).toAlignedRect();
}

void MapWidget::setZoomDisplay(std::function<void(const QString&)> setter)
{
	this->zoom_display = setter;
	updateZoomDisplay();
}

void MapWidget::setCursorposLabel(QLabel* cursorpos_label)
{
	this->cursorpos_label = cursorpos_label;
}

void MapWidget::updateZoomDisplay()
{
	if (zoom_display)
	{
		auto zoom = view->getZoom();
		if (pinching)
			zoom *= pinching_factor;
		zoom_display(tr("%1x", "Zoom factor").arg(zoom, 0, 'g', 3));
	}
}

void MapWidget::setCoordsDisplay(CoordsType type)
{
	coords_type = type;
	updateCursorposLabel(last_cursor_pos);
}

void MapWidget::updateCursorposLabel(const MapCoordF& pos)
{
	last_cursor_pos = pos;
	
	if (!cursorpos_label)
		return;
	
	if (coords_type == MAP_COORDS)
	{
		cursorpos_label->setText( QStringLiteral("%1 %2 (%3)").
		  arg(locale().toString(pos.x(), 'f', 2),
		      locale().toString(-pos.y(), 'f', 2),
		      tr("mm", "millimeters")) );
	}
	else
	{
		const Georeferencing& georef = view->getMap()->getGeoreferencing();
		bool ok = true;
		if (coords_type == PROJECTED_COORDS)
		{
			const QPointF projected_point(georef.toProjectedCoords(pos));
			if (qAbs(georef.getCombinedScaleFactor() - 1.0) < 0.02)
			{
				// Grid unit differs less than 2% from meter.
				cursorpos_label->setText(
				  QStringLiteral("%1 %2 (%3)").
				  arg(QString::number(projected_point.x(), 'f', 0),
				      QString::number(projected_point.y(), 'f', 0),
				      tr("m", "meters"))
				); 
			}
			else
			{
				cursorpos_label->setText(
				  QStringLiteral("%1 %2").
				  arg(QString::number(projected_point.x(), 'f', 0),
				      QString::number(projected_point.y(), 'f', 0))
				); 
			}
		}
		else if (coords_type == GEOGRAPHIC_COORDS)
		{
			const LatLon lat_lon(georef.toGeographicCoords(pos, &ok));
			cursorpos_label->setText(
			  QString::fromUtf8("%1° %2°").
			  arg(locale().toString(lat_lon.latitude(), 'f', 6),
			      locale().toString(lat_lon.longitude(), 'f', 6))
			); 
		}
		else if (coords_type == GEOGRAPHIC_COORDS_DMS)
		{
			const LatLon lat_lon(georef.toGeographicCoords(pos, &ok));
			cursorpos_label->setText(
			  QStringLiteral("%1 %2").
			  arg(georef.degToDMS(lat_lon.latitude()),
			      georef.degToDMS(lat_lon.longitude()))
			); 
		}
		else
		{
			// shall never happen
			ok = false;
		}
		
		if (!ok)
			cursorpos_label->setText(tr("Error"));
	}
}

int MapWidget::getTimeSinceLastInteraction()
{
	if (current_pressed_buttons != 0)
		return 0;
	else
		return last_mouse_release_time.msecsTo(QTime::currentTime());
}

void MapWidget::setGPSDisplay(GPSDisplay* gps_display)
{
	this->gps_display = gps_display;
}

void MapWidget::setTemporaryMarkerDisplay(GPSTemporaryMarkers* marker_display)
{
	this->marker_display = marker_display;
}

QWidget* MapWidget::getContextMenu()
{
	return context_menu;
}

QSize MapWidget::sizeHint() const
{
    return QSize(640, 480);
}

void MapWidget::showHelpMessage(QPainter* painter, const QString& text) const
{
	painter->fillRect(rect(), QColor(Qt::gray));
	
	QFont font = painter->font();
	int pixel_size = font.pixelSize();
	if (pixel_size > 0)
	{
		font.setPixelSize(pixel_size * 2);
	}
	else
	{
		pixel_size = font.pointSize();
		font.setPointSize(pixel_size * 2);
	}
	font.setBold(true);
	painter->setFont(font);
	painter->drawText(QRect(0, 0, width(), height()), Qt::AlignCenter, text);
}

bool MapWidget::event(QEvent* event)
{
	switch (event->type())
	{
	case QEvent::Gesture:
		gestureEvent(static_cast<QGestureEvent*>(event));
		return event->isAccepted();
		
	case QEvent::TouchBegin:
	case QEvent::TouchUpdate:
	case QEvent::TouchEnd:
	case QEvent::TouchCancel:
		if (static_cast<QTouchEvent*>(event)->points().count() >= 2)
			return true;
		break;
		
	case QEvent::KeyPress:
		// No focus changing in QWidget::event if Tab is handled by tool.
		if (static_cast<QKeyEvent*>(event)->key() == Qt::Key_Tab
		    && keyPressEventFilter(static_cast<QKeyEvent*>(event)))
			return true;
		break;
		
	default:
		; // nothing
	}
	
    return QWidget::event(event);
}

void MapWidget::gestureEvent(QGestureEvent* event)
{
	if (tool && tool->gestureEvent(event, this))
	{
		event->accept();
		return;
	}
	
	if (QGesture* gesture = event->gesture(Qt::PinchGesture))
	{
		QPinchGesture* pinch = static_cast<QPinchGesture *>(gesture);
		QPoint center = pinch->centerPoint().toPoint();
		qreal factor = pinch->totalScaleFactor();
		switch (pinch->state())
		{
		case Qt::GestureStarted:
			if (dragging)
				cancelDragging();
			if (pinching)
				cancelPinching();
			if (tool)
				tool->gestureStarted();
			factor = startPinching(center);
			pinch->setTotalScaleFactor(factor);
			break;
		case Qt::GestureUpdated:
			updatePinching(center, factor);
			break;
		case Qt::GestureFinished:
			finishPinching(center, factor);
			break;
		case Qt::GestureCanceled:
			cancelPinching();
			break;
		default:
			Q_UNREACHABLE(); // unknown gesture state
		}
		event->accept();
	}
	else
	{
		event->ignore();
	}
}

void MapWidget::paintEvent(QPaintEvent* event)
{
	// Draw on the widget
	QPainter painter(this);
	QRect exposed = event->rect();
	
	if (!view)
	{
		painter.fillRect(exposed, QColor(Qt::gray));
		return;
	}
	
	// No colors, symbols, or objects? Provide a little help message ...
	bool no_contents = view->getMap()->getNumObjects() == 0 && view->getMap()->getNumTemplates() == 0 && !view->isGridVisible();
	
	QTransform transform = painter.worldTransform();
	
	// Update all dirty caches
	// TODO: It would be an idea to do these updates in a background thread and use the old caches in the meantime
	updateAllDirtyCaches();
	
	QRect target = exposed;
	if (pinching)
	{
		// Just draw the scaled map and templates
		painter.fillRect(exposed, QColor(Qt::gray));
		painter.translate(pinching_center.x(), pinching_center.y());
		painter.scale(pinching_factor, pinching_factor);
		painter.translate(-drag_start_pos.x(), -drag_start_pos.y());
	}
	else if (pan_offset != QPoint())
	{
		// Tiled compositor covers all areas — no gray bar fill needed.
		target.translate(pan_offset);
	}
	
	// Unified scene compositing.
	// To avoid jiggle, never mix fallback and fresh tiles in the same
	// frame — either show ALL fallback or ALL fresh. The switch happens
	// atomically when all visible tiles are clean.
	QRect composite_clip = rect();

	bool all_clean = !has_zoom_fallback || scene_store.allTilesClean(viewportToView(composite_clip));

	if (has_zoom_fallback && !all_clean)
	{
		// Still waiting for fresh tiles — show scaled fallback only.
		compositeFallbackTiles(scene_store, painter, composite_clip);
	}
	else if (!scene_store.isEmpty())
	{
		// All fresh tiles ready (or no fallback) — show them.
		compositeStoreTiles(scene_store, painter, composite_clip, Qt::white);
	}
	else if (show_help && no_contents)
	{
		painter.save();
		painter.setTransform(transform);
		if (view->getMap()->getNumColors() == 0)
			showHelpMessage(&painter, tr("Empty map!\n\nStart by defining some colors:\nSelect Symbols -> Color window to\nopen the color dialog and\ndefine the colors there."));
		else if (view->getMap()->getNumSymbols() == 0)
			showHelpMessage(&painter, tr("No symbols!\n\nNow define some symbols:\nRight-click in the symbol bar\nand select \"New symbol\"\nto create one."));
		else
			showHelpMessage(&painter, tr("Ready to draw!\n\nStart drawing or load a base map.\nTo load a base map, click\nTemplates -> Open template...") + QLatin1String("\n\n") + tr("Hint: Hold the middle mouse button to drag the map,\nzoom using the mouse wheel, if available."));
		painter.restore();
	}
	else
	{
		painter.fillRect(composite_clip, Qt::white);
	}
	
	//painter.setClipRect(exposed);
	
	// Show current drawings
	if (activity_dirty_rect.isValid())
		activity->draw(&painter, this);
	
	if (drawing_dirty_rect.isValid())
		tool->draw(&painter, this);
	
	
	// Draw temporary GPS marker display
	if (marker_display)
		marker_display->paint(&painter);
	
	// Draw GPS display
	if (gps_display)
		gps_display->paint(&painter);
	
	// Draw touch cursor
	if (touch_cursor && tool && tool->usesTouchCursor())
		touch_cursor->paint(&painter);
	
	
	painter.setWorldTransform(transform, false);
}

void MapWidget::resizeEvent(QResizeEvent* event)
{
	// Tiled backing stores: existing tiles remain valid (view-space anchored).
	// New viewport edges will naturally request tiles that are dirty or missing.
	// Just mark the legacy dirty rects so updateAllDirtyCaches knows to run.
	scene_dirty_rect = rect();
	
	for (QObject* const child : children())
	{
		if (QWidget* child_widget = qobject_cast<ActionGridBar*>(child))
		{
			child_widget->resize(event->size().width(), child_widget->sizeHint().height());
		}
		else if (QWidget* child_widget = qobject_cast<KeyButtonBar*>(child))
		{
			QSize size = child_widget->sizeHint();
			QRect map_widget_rect = rect();
			child_widget->setGeometry(
				qMax(0, qRound(map_widget_rect.center().x() - 0.5f * size.width())),
				qMax(0, map_widget_rect.bottom() - size.height()),
				qMin(size.width(), map_widget_rect.width()),
				qMin(size.height(), map_widget_rect.height()) );
		}
	}
	
	QWidget::resizeEvent(event);
	scheduleRenderContextUpdate();
}

void MapWidget::mousePressEvent(QMouseEvent* event)
{
	current_pressed_buttons = event->buttons();
	if (touch_cursor && tool && tool->usesTouchCursor())
	{
		touch_cursor->mousePressEvent(event);
		if (event->type() == QEvent::MouseMove)
		{
			_mouseMoveEvent(event);
			return;
		}
	}
	_mousePressEvent(event);
}

void MapWidget::_mousePressEvent(QMouseEvent* event)
{
	if (dragging || pinching)
	{
		event->accept();
		return;
	}
	
	if (tool && tool->mousePressEvent(event, view->viewToMapF(viewportToView(event->pos())), this))
	{
		event->accept();
		return;
	}
	
	if (event->button() == Qt::MiddleButton)
	{
		startDragging(event->pos());
		event->accept();
	}
	else if (event->button() == Qt::RightButton)
	{
		if (!context_menu->isEmpty())
			context_menu->popup(event->globalPosition().toPoint());
	}
}

void MapWidget::mouseMoveEvent(QMouseEvent* event)
{
	if (touch_cursor && tool && tool->usesTouchCursor())
	{
		if (!touch_cursor->mouseMoveEvent(event))
			return;
	}
	_mouseMoveEvent(event);
}

void MapWidget::_mouseMoveEvent(QMouseEvent* event)
{
	if (pinching)
	{
		event->accept();
		return;
	}
	else if (dragging)
	{
		updateDragging(event->pos());
		return;
	}
	else
    {
		updateCursorposLabel(view->viewToMapF(viewportToView(event->pos())));
    }
	
	if (tool && tool->mouseMoveEvent(event, view->viewToMapF(viewportToView(event->pos())), this))
	{
		event->accept();
		return;
	}
}

void MapWidget::mouseReleaseEvent(QMouseEvent* event)
{
	current_pressed_buttons = event->buttons();
	last_mouse_release_time = QTime::currentTime();
	if (touch_cursor && tool && tool->usesTouchCursor())
	{
		if (!touch_cursor->mouseReleaseEvent(event))
			return;
	}
	_mouseReleaseEvent(event);
}

void MapWidget::_mouseReleaseEvent(QMouseEvent* event)
{
	if (dragging)
	{
		finishDragging(event->pos());
		event->accept();
		return;
	}
	
	if (tool && tool->mouseReleaseEvent(event, view->viewToMapF(viewportToView(event->pos())), this))
	{
		event->accept();
		return;
	}
}

void MapWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
	if (touch_cursor && tool && tool->usesTouchCursor())
	{
		if (!touch_cursor->mouseDoubleClickEvent(event))
			return;
	}
	_mouseDoubleClickEvent(event);
}

void MapWidget::_mouseDoubleClickEvent(QMouseEvent* event)
{
	if (tool && tool->mouseDoubleClickEvent(event, view->viewToMapF(viewportToView(event->pos())), this))
	{
		event->accept();
		return;
	}
	
	QWidget::mouseDoubleClickEvent(event);
}

void MapWidget::wheelEvent(QWheelEvent* event)
{
	auto delta_y = event->angleDelta().y();
	if (delta_y != 0)
	{
		if (view)
		{
			auto degrees = delta_y / 8.0;
			auto num_steps = degrees / 15.0;
			auto cursor_pos_view = viewportToView(event->position().toPoint());
			bool preserve_cursor_pos = (event->modifiers() & Qt::ControlModifier) == 0;
			if (num_steps < 0 && !Settings::getInstance().getSettingCached(Settings::MapEditor_ZoomOutAwayFromCursor).toBool())
				preserve_cursor_pos = !preserve_cursor_pos;
			if (preserve_cursor_pos)
			{
				view->zoomSteps(num_steps, cursor_pos_view);
			}
			else
			{
				view->zoomSteps(num_steps);
				updateCursorposLabel(view->viewToMapF(cursor_pos_view));
			}

			// Send a mouse move event to the current tool as zooming out can move the mouse position on the map
			if (tool)
			{
				QMouseEvent mouse_event{ QEvent::HoverMove, event->position().toPoint(), Qt::NoButton, QApplication::mouseButtons(), Qt::NoModifier };
				tool->mouseMoveEvent(&mouse_event, view->viewToMapF(cursor_pos_view), this);
			}
		}

		event->accept();
	}
	else
		event->ignore();
}

void MapWidget::leaveEvent(QEvent* event)
{
	if (tool)
		tool->leaveEvent(event);
}

bool MapWidget::keyPressEventFilter(QKeyEvent* event)
{
	if (tool && tool->keyPressEvent(event))
	{
		return true;
	}
	
	switch (event->key())
	{
	case Qt::Key_F6:
		if (dragging)
			finishDragging(mapFromGlobal(QCursor::pos()));
		else
			startDragging(mapFromGlobal(QCursor::pos()));
		return true;
		
	case Qt::Key_Up:
		moveMap(0, -1);
		return true;
		
	case Qt::Key_Down:
		moveMap(0, 1);
		return true;
		
	case Qt::Key_Left:
		moveMap(-1, 0);
		return true;
		
	case Qt::Key_Right:
		moveMap(1, 0);
		return true;
		
	default:
		return false;
	}
}

bool MapWidget::keyReleaseEventFilter(QKeyEvent* event)
{
	if (tool && tool->keyReleaseEvent(event))
	{
		return true; // NOLINT
	}
	
	return false;
}

QVariant MapWidget::inputMethodQuery(Qt::InputMethodQuery property) const
{
	return inputMethodQuery(property, {});
}

QVariant MapWidget::inputMethodQuery(Qt::InputMethodQuery property, const QVariant& argument) const
{
	QVariant result;
	if (tool)
		result = tool->inputMethodQuery(property, argument);
	if (!result.isValid())
		result = QWidget::inputMethodQuery(property);
	return result;
}

void MapWidget::inputMethodEvent(QInputMethodEvent* event)
{
	if (tool)
		tool->inputMethodEvent(event);
}

void MapWidget::enableTouchCursor(bool enabled)
{
	if (enabled && !touch_cursor)
	{
		touch_cursor.reset(new TouchCursor(this));
	}
	else if (!enabled && touch_cursor)
	{
		touch_cursor->updateMapWidget(false);
		touch_cursor.reset(nullptr);
	}
}

void MapWidget::focusOutEvent(QFocusEvent* event)
{
	if (tool)
		tool->focusOutEvent(event);
	QWidget::focusOutEvent(event);
}

void MapWidget::contextMenuEvent(QContextMenuEvent* event)
{
	if (event->reason() == QContextMenuEvent::Mouse)
	{
		// HACK: Ignore context menu events caused by the mouse, because right click
		// events need to be sent to the current tool first.
		event->ignore();
		return;
	}
	
	if (!context_menu->isEmpty())
		context_menu->popup(event->globalPos());
	
	event->accept();
}

void MapWidget::updatePlaceholder()
{
	if (!show_help)
		return;
	
	auto const* map = view->getMap();
	if (map->getNumObjects() > 1)
		return;
	
	if (map->getNumColors() < 2
	    || map->getNumSymbols() < 2
	    || map->getNumTemplates() < 2)
	{
		update();
	}
}

bool MapWidget::containsVisibleTemplate(int first_template, int last_template) const
{
	if (first_template > last_template)
		return false;	// no template visible
		
	Map* map = view->getMap();
	for (int i = first_template; i <= last_template; ++i)
	{
		if (view->isTemplateVisible(map->getTemplate(i)))
			return true;
	}
	
	return false;
}

inline
bool MapWidget::isAboveTemplateVisible() const
{
	return containsVisibleTemplate(view->getMap()->getFirstFrontTemplate(), view->getMap()->getNumTemplates() - 1);
}

inline
bool MapWidget::isBelowTemplateVisible() const
{
	return containsVisibleTemplate(0, view->getMap()->getFirstFrontTemplate() - 1);
}

void MapWidget::updateAllDirtyCaches()
{
	last_rendered_transform = view->worldTransform();

	if (scene_dirty_rect.isValid() || !scene_store.isEmpty())
		updateSceneTiles();

	// Discard fallback once all visible scene tiles are rendered.
	if (has_zoom_fallback)
	{
		QRectF vr = viewportToView(rect());
		if (scene_store.allTilesClean(vr))
		{
			scene_store.clearFallback();
			has_zoom_fallback = false;
		}
	}
}

void MapWidget::updateSceneTiles()
{
	QRectF view_rect = viewportToView(rect());
	int overscan = scene_store.tileSize();
	QRectF padded = view_rect.adjusted(-overscan, -overscan, overscan, overscan);

	int col_min, col_max, row_min, row_max;
	scene_store.tilesForViewRect(padded, col_min, col_max, row_min, row_max);

	if (scene_dirty_rect.isValid())
	{
		scene_store.dirtyViewRect(viewportToView(scene_dirty_rect));
		scene_dirty_rect.setWidth(-1);
	}

	Map* map = view->getMap();
	int ts = scene_store.tileSize();

	bool use_antialiasing = force_antialiasing || Settings::getInstance().getSettingCached(Settings::MapDisplay_Antialiasing).toBool();

	// Take an exclusive lock on the rendering data.  This waits for any
	// in-progress worker renders to finish before we mutate objects,
	// renderables, and the spatial index.  Workers hold a shared (read)
	// lock during their render, so multiple workers can render
	// concurrently, but mutations are always conflict-free.
	{
		std::unique_lock lock(tile_scheduler->renderDataMutex());
		map->updateObjects();
		map->mapRenderables().prepareDraw();
	}

	// Capture template visibility state for the render function.
	bool templates_hidden = view->areAllTemplatesHidden();
	int first_front_template = map->getFirstFrontTemplate();
	int num_templates = map->getNumTemplates();
	bool below_visible = !templates_hidden && isBelowTemplateVisible();
	bool above_visible = !templates_hidden && isAboveTemplateVisible();
	const MapView* snap_view = view;
	auto map_vis = view->effectiveMapVisibility();

	RenderConfig::Options options(RenderConfig::Screen | RenderConfig::HelperSymbols);
	if (!use_antialiasing)
		options |= RenderConfig::DisableAntialiasing | RenderConfig::ForceMinSize;

	const MapRenderables* mr = &map->mapRenderables();

	// Collect dirty tiles and dispatch to worker threads.
	std::vector<TileRenderJob> jobs;
	int current_gen = tile_scheduler->generation();

	TileRenderSnapshot snapshot;
	snapshot.world_transform = view->worldTransform();
	snapshot.zoom_factor = view->calculateFinalZoomFactor();
	snapshot.antialiasing = use_antialiasing;
	snapshot.overprinting = view->isOverprintingSimulationEnabled();
	snapshot.grid_visible = view->isGridVisible();
	snapshot.generation = current_gen;

	// Build the render function fresh each dispatch so it always
	// reflects the current template count, visibility, etc.
	snapshot.render_func =
		[map, mr, options, snap_view, below_visible, above_visible,
		 first_front_template, num_templates, map_vis](
			QPainter& painter, const QRectF& tile_map_rect,
			const TileRenderSnapshot& snap) {

			// 1. Below-templates (white background)
			painter.fillRect(painter.window(), Qt::white);
			if (below_visible)
				map->drawTemplates(&painter, tile_map_rect, 0, first_front_template - 1, snap_view, true);

			// 2. Map objects
			if (map_vis.visible)
			{
				qreal saved = painter.opacity();
				painter.setOpacity(map_vis.opacity);

				RenderConfig config = { *map, tile_map_rect, snap.zoom_factor, options, 1.0 };

#ifndef Q_OS_ANDROID
				if (snap.overprinting)
					mr->drawOverprintingSimulation(&painter, config);
				else
#endif
					mr->draw(&painter, config);

				painter.setOpacity(saved);
			}

			// 3. Above-templates
			if (above_visible)
				map->drawTemplates(&painter, tile_map_rect, first_front_template, num_templates - 1, snap_view, true);
		};

	for (int row = row_min; row <= row_max; ++row)
	{
		for (int col = col_min; col <= col_max; ++col)
		{
			TileKey key{col, row};
			BackingTile& tile = scene_store.ensureTile(key);
			if (tile.state != BackingTile::Dirty)
				continue;

			tile.state = BackingTile::InFlight;

			TileRenderJob job;
			job.key = key;
			job.tile_view_rect = scene_store.tileViewRect(key);
			job.tile_map_rect = view->calculateViewedRect(job.tile_view_rect);
			job.image = QImage(ts, ts, QImage::Format_ARGB32_Premultiplied);
			job.generation = current_gen;

			jobs.push_back(std::move(job));
		}
	}

	if (!jobs.empty())
		tile_scheduler->submit(jobs, snapshot);

	int evict_margin = overscan * 2;
	scene_store.evict(view_rect.adjusted(-evict_margin, -evict_margin, evict_margin, evict_margin));
}


void MapWidget::installSceneTileResults()
{
	auto results = tile_scheduler->collectResults();
	if (results.empty())
		return;

	for (auto& result : results)
	{
		BackingTile* tile = scene_store.tile(result.key);
		if (tile && tile->state == BackingTile::InFlight)
		{
			tile->image = std::move(result.image);
			tile->state = BackingTile::Clean;
		}
	}

	// Repaint to show the newly completed tiles.
	update();
}


void MapWidget::compositeStoreTiles(const BackingStore& store, QPainter& painter, const QRect& clip_rect, const QColor& fallback_color)
{
	QPointF viewport_origin(width() / 2.0 + pan_offset.x(),
	                        height() / 2.0 + pan_offset.y());

	QRectF clip_view = viewportToView(clip_rect);

	int col_min, col_max, row_min, row_max;
	store.tilesForViewRect(clip_view, col_min, col_max, row_min, row_max);

	bool has_fallback = store.hasFallback();

	for (int row = row_min; row <= row_max; ++row)
	{
		for (int col = col_min; col <= col_max; ++col)
		{
			TileKey key{col, row};
			const BackingTile* tile = store.tile(key);

			QRect tile_vp = store.tileViewportRect(key, viewport_origin);
			QRect draw_rect = tile_vp.intersected(clip_rect);
			if (draw_rect.isEmpty())
				continue;

			if (tile && !tile->image.isNull())
			{
				// Draw the tile — clean content or stale content from a
				// previous render.  Showing stale content avoids flashing
				// white while fresh tiles are being rendered.
				QRect src_rect(draw_rect.x() - tile_vp.x(),
				               draw_rect.y() - tile_vp.y(),
				               draw_rect.width(), draw_rect.height());
				painter.drawImage(draw_rect, tile->image, src_rect);
			}
			else if (fallback_color.alpha() > 0 && !has_fallback)
			{
				// No image yet (brand-new tile) — fill with fallback
				// color so no gray or stale frame-buffer shows through.
				painter.fillRect(draw_rect, fallback_color);
			}
		}
	}
}


void MapWidget::compositeFallbackTiles(const BackingStore& store, QPainter& painter, const QRect& clip_rect)
{
	if (!store.hasFallback())
		return;

	auto* fb = store.fallbackData();
	int ts = store.tileSize();

	QPointF viewport_origin(width() / 2.0 + pan_offset.x(),
	                        height() / 2.0 + pan_offset.y());

	// Transform from old view space to new view space.
	// worldTransform maps map→view.  We need old_view → map → new_view.
	// In Qt's row-vector convention (A*B).map(p) = B(A(p)), so:
	//   old_to_new = old_wt.inverted() * new_wt
	// which maps: p → old_wt_inv(p) = map_point → new_wt(map_point) = new_view.
	bool invertible;
	QTransform old_to_new = fallback_transform.inverted(&invertible) * view->worldTransform();
	if (!invertible)
		return;

	for (auto it = fb->tiles.begin(); it != fb->tiles.end(); ++it)
	{
		const TileKey& key = it->first;
		const BackingTile& tile = it->second;

		if (tile.image.isNull() || !tile.clean())
			continue;

		// Tile origin in old view space (with old grid offset)
		QPointF tile_origin(key.col * ts + fb->grid_offset.x(),
		                    key.row * ts + fb->grid_offset.y());

		// Map tile corners from old view space → new view space → viewport
		QPointF new_tl = old_to_new.map(tile_origin) + viewport_origin;
		QPointF new_br = old_to_new.map(tile_origin + QPointF(ts, ts)) + viewport_origin;

		QRectF dest = QRectF(new_tl, new_br).normalized();
		if (!dest.intersects(QRectF(clip_rect)))
			continue;

		// drawImage with dest QRectF handles scaling automatically.
		painter.drawImage(dest, tile.image);
	}
}


void MapWidget::adjustAllGridOffsets(QPointF delta)
{
	scene_store.adjustGridOffset(delta);
}


}  // namespace OpenOrienteering
