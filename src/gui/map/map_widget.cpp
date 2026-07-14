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

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
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
#include "render/qt_render_bridge.h"
#include "render/template_layer_planner.h"
#include "presentation/vello_canvas.h"
#include "gui/touch_cursor.h"
#include "gui/map/map_editor_activity.h"
#include "gui/widgets/action_grid_bar.h"
#include "gui/widgets/key_button_bar.h"
#include "gui/widgets/pie_menu.h"
#include "sensors/gps_display.h"
#include "sensors/gps_temporary_markers.h"
#include "templates/template.h"
#include "tools/tool.h"
#include "util/util.h"

class QGesture;
// IWYU pragma: no_forward_declare QPinchGesture


#ifdef __clang_analyzer__
#define singleShot(A, B, C) singleShot(A, B, #C) // NOLINT 
#endif


namespace OpenOrienteering {

namespace {

template<typename Handler>
void dispatchTouchCursorEvent(const TouchCursor::MouseEventTranslation& translation,
                              QMouseEvent* source,
                              const QWidget& target,
                              Handler&& handler)
{
	using Action = TouchCursor::MouseEventTranslation::Action;
	if (translation.action == Action::Discard)
		return;
	if (translation.action == Action::PassThrough)
	{
		handler(source);
		return;
	}

	QMouseEvent translated {
		translation.type,
		translation.position,
		target.mapToGlobal(translation.position),
		translation.button,
		translation.buttons,
		source->modifiers(),
		source->pointingDevice()
	};
	translated.setTimestamp(source->timestamp());
	handler(&translated);
}

}  // namespace

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
 , vello_canvas(new presentation::VelloCanvas(this))
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
	vello_canvas->setGeometry(rect());
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
				observeTemplate(map->getTemplate(i));
			connect(map, &Map::templateAdded, this, [this](int, Template* temp) {
				observeTemplate(temp);
				scheduleRenderContextUpdate();
			});
			connect(map, &Map::templateChanged, this, [this](int, Template* temp) {
				observeTemplate(temp);
				scheduleRenderContextUpdate();
			});
			connect(map, &Map::templateDeleted, this,
			        [this](int, const Template* temp) {
				disconnect(temp, nullptr, this, nullptr);
				scheduleRenderContextUpdate();
			});
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


void MapWidget::applyMapTransform(render::OverlaySceneBuilder* painter) const
{
	auto const origin = mapToViewport(QPointF(0, 0));
	auto const x_axis = mapToViewport(QPointF(1, 0));
	auto const y_axis = mapToViewport(QPointF(0, 1));
	painter->setWorldTransform(QTransform(
		x_axis.x() - origin.x(), x_axis.y() - origin.y(),
		y_axis.x() - origin.x(), y_axis.y() - origin.y(),
		origin.x(), origin.y()
	));
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

void MapWidget::observeTemplate(Template* temp)
{
	connect(temp, &Template::templateStateChanged,
	        this, &MapWidget::scheduleRenderContextUpdate,
	        Qt::UniqueConnection);
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

	auto const context = currentViewRenderContext();
	auto* map = view->getMap();
	for (int i = 0; i < map->getNumTemplates(); ++i)
	{
		auto* temp = map->getTemplate(i);
		if (temp->getTemplateState() == Template::Loaded && view->isTemplateVisible(temp))
			temp->updateRenderContext(context);
	}
}

ViewRenderContext MapWidget::currentViewRenderContext() const
{
	Q_ASSERT(view);
	return {
		view->calculateViewedRect(viewportToView(rect())),
		view->getZoom(),
	};
}

void MapWidget::viewChanged(MapView::ChangeFlags changes)
{
	setDrawingBoundingBox(drawing_dirty_rect_map, drawing_dirty_rect_border, true);
	setActivityBoundingBox(activity_dirty_rect_map, activity_dirty_rect_border, true);
	updateEverything();
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
			auto const template_area = temp->calculateTemplateBoundingBox();
			markTemplateAreaDirty(getMapView()->calculateViewBoundingBox(template_area),
			                      temp->getTemplateBoundingBoxPixelBorder());
		
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
	view->finishPanning(cursor_pos - drag_start_pos);
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

void MapWidget::markTemplateAreaDirty(const QRectF& view_rect, int pixel_border)
{
	QRectF viewport_rect = viewToViewport(view_rect);
	QRect integer_rect = QRect(viewport_rect.left() - (1+pixel_border), viewport_rect.top() - (1+pixel_border),
							   viewport_rect.width() + 2*(1+pixel_border), viewport_rect.height() + 2*(1+pixel_border));
	
	if (integer_rect.intersects(rect()))
		update(integer_rect);
}

void MapWidget::markObjectAreaDirty(const QRectF& map_rect)
{
	updateDrawing(map_rect, 0);
}

void MapWidget::setDrawingBoundingBox(QRectF map_rect, int pixel_border, bool do_update)
{
	auto redraw = drawing_dirty_rect;
	if (map_rect.isValid())
	{
		drawing_dirty_rect_map = map_rect;
		drawing_dirty_rect_border = pixel_border;
		drawing_dirty_rect = calculateViewportBoundingBox(map_rect, pixel_border).intersected(rect());
		redraw = redraw.united(drawing_dirty_rect);
	}
	else
	{
		drawing_dirty_rect_map = {};
		drawing_dirty_rect = {};
	}
	if (do_update && redraw.isValid())
		update(redraw);
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
	auto redraw = activity_dirty_rect;
	if (map_rect.isValid())
	{
		activity_dirty_rect_map = map_rect;
		activity_dirty_rect_border = pixel_border;
		activity_dirty_rect = calculateViewportBoundingBox(map_rect, pixel_border).intersected(rect());
		redraw = redraw.united(activity_dirty_rect);
	}
	else
	{
		activity_dirty_rect_map = {};
		activity_dirty_rect = {};
	}
	if (do_update && redraw.isValid())
		update(redraw);
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
	update();
}

void MapWidget::updateEverythingInRect(const QRect& dirty_rect)
{
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

void MapWidget::showHelpMessage(render::OverlaySceneBuilder* painter, const QString& text) const
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
	Q_UNUSED(event)
	if (overlay_revision == std::numeric_limits<render::Revision>::max())
		qFatal("Map viewport revision space exhausted");

	auto const viewport_bounds = render::Rect {
		0, 0, double(std::max(0, width())), double(std::max(0, height()))
	};
	if (!view)
	{
		overlay_scene_builder.begin(overlay_revision++, viewport_bounds);
		render::FrameRequest request;
		request.view = {
			std::uint32_t(std::max(0, width())),
			std::uint32_t(std::max(0, height())),
			std::max(1.0, double(devicePixelRatioF())),
			{},
		};
		request.above_map.push_back({
			overlay_scene_builder.finish(),
			render::BlendMode::SourceOver, 1, false,
			render::VectorPass::Space::Viewport,
		});
		auto frame = frame_planner.plan(request);
		vello_canvas->setBackground(render::fromQColor(QColor(Qt::gray)));
		vello_canvas->setFrame(std::move(frame));
		return;
	}

	auto* map = view->getMap();
	auto no_contents = map->getNumObjects() == 0
	                && map->getNumTemplates() == 0
	                && !view->isGridVisible();

	QTransform interaction_transform;
	if (pinching)
	{
		interaction_transform.translate(pinching_center.x(), pinching_center.y());
		interaction_transform.scale(pinching_factor, pinching_factor);
		interaction_transform.translate(-drag_start_pos.x(), -drag_start_pos.y());
	}
	auto viewport_point = [this, &interaction_transform](QPointF point) {
		return interaction_transform.map(mapToViewport(point));
	};
	auto const origin = viewport_point({ 0, 0 });
	auto const x_axis = viewport_point({ 1, 0 });
	auto const y_axis = viewport_point({ 0, 1 });
	auto const world_to_viewport = render::Transform {
		x_axis.x() - origin.x(), x_axis.y() - origin.y(),
		y_axis.x() - origin.x(), y_axis.y() - origin.y(),
		origin.x(), origin.y(),
	};

	auto const map_view_rect = view->calculateViewedRect(viewportToView(rect()));
	RenderConfig::Options options(RenderConfig::Screen | RenderConfig::HelperSymbols);
	auto use_antialiasing = force_antialiasing
	                         || Settings::getInstance()
	                              .getSettingCached(Settings::MapDisplay_Antialiasing)
	                              .toBool();
	if (!use_antialiasing)
		options |= RenderConfig::DisableAntialiasing | RenderConfig::ForceMinSize;
	auto const map_visibility = view->effectiveMapVisibility();
	auto const render_request = render::RenderRequest {
		render::fromQRectF(map_view_rect),
		view->calculateFinalZoomFactor() * (pinching ? pinching_factor : 1),
		options,
		map_visibility.visible ? double(map_visibility.opacity) : 0,
	};

	render::TemplateLayerPlan template_layers;
	if (!view->areAllTemplatesHidden())
	{
		template_layers = template_layer_planner.plan(
			*map, *view, render::fromQRectF(map_view_rect),
			render_request.scaling, true
		);
	}

	overlay_scene_builder.begin(overlay_revision++, viewport_bounds);
	if (show_help && no_contents)
	{
		if (map->getNumColors() == 0)
			showHelpMessage(&overlay_scene_builder, tr("Empty map!\n\nStart by defining some colors:\nSelect Symbols -> Color window to\nopen the color dialog and\ndefine the colors there."));
		else if (map->getNumSymbols() == 0)
			showHelpMessage(&overlay_scene_builder, tr("No symbols!\n\nNow define some symbols:\nRight-click in the symbol bar\nand select \"New symbol\"\nto create one."));
		else
			showHelpMessage(&overlay_scene_builder, tr("Ready to draw!\n\nStart drawing or load a base map.\nTo load a base map, click\nTemplates -> Open template...") + QLatin1String("\n\n") + tr("Hint: Hold the middle mouse button to drag the map,\nzoom using the mouse wheel, if available."));
	}
	if (activity_dirty_rect.isValid() && activity)
		activity->draw(&overlay_scene_builder, this);
	if (drawing_dirty_rect.isValid() && tool)
		tool->draw(&overlay_scene_builder, this);
	if (marker_display)
		marker_display->paint(&overlay_scene_builder);
	if (gps_display)
		gps_display->paint(&overlay_scene_builder);
	if (touch_cursor && tool && tool->usesTouchCursor())
		touch_cursor->paint(&overlay_scene_builder);
	auto overlay = overlay_scene_builder.finish();

	render::FrameRequest frame_request {
		{
			std::uint32_t(std::max(0, width())),
			std::uint32_t(std::max(0, height())),
			std::max(1.0, double(devicePixelRatioF())),
			world_to_viewport,
		},
		render_request,
		false,
	};
#ifndef Q_OS_ANDROID
	frame_request.simulate_overprinting = view->isOverprintingSimulationEnabled();
#endif
	frame_request.below_map = std::move(template_layers.below_map);
	if (view->isGridVisible())
	{
		frame_request.above_map.push_back({
			map->getGrid().buildRenderIR(map_view_rect, map, render_request.scaling,
			                             overlay_revision++),
		});
	}
	frame_request.above_map.insert(
		frame_request.above_map.end(),
		std::make_move_iterator(template_layers.above_map.begin()),
		std::make_move_iterator(template_layers.above_map.end())
	);
	frame_request.above_map.push_back({
		std::move(overlay),
		render::BlendMode::SourceOver, 1, false,
		render::VectorPass::Space::Viewport,
	});
	frame_request.raster_complete = template_layers.complete;

	auto const snapshot = map->publishRenderSnapshot();
	auto frame = frame_planner.plan(*snapshot, frame_request);
	vello_canvas->setBackground(render::fromQColor(
		show_help && no_contents ? QColor(Qt::gray) : QColor(Qt::white)
	));
	vello_canvas->setFrame(std::move(frame));
}

void MapWidget::resizeEvent(QResizeEvent* event)
{
	vello_canvas->setGeometry(QRect(QPoint(), event->size()));
	
	for (QObject* const child : children())
	{
		if (QWidget* child_widget = qobject_cast<ActionGridBar*>(child))
		{
			child_widget->resize(event->size().width(), child_widget->sizeHint().height());
			child_widget->setAttribute(Qt::WA_NativeWindow);
			child_widget->raise();
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
			child_widget->setAttribute(Qt::WA_NativeWindow);
			child_widget->raise();
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
		auto translation = touch_cursor->mousePressEvent(*event);
		dispatchTouchCursorEvent(translation, event, *this, [this](QMouseEvent* translated) {
			if (translated->type() == QEvent::MouseMove)
				_mouseMoveEvent(translated);
			else
				_mousePressEvent(translated);
		});
		return;
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
		auto translation = touch_cursor->mouseMoveEvent(*event);
		dispatchTouchCursorEvent(translation, event, *this, [this](QMouseEvent* translated) {
			_mouseMoveEvent(translated);
		});
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
		auto translation = touch_cursor->mouseReleaseEvent(*event);
		dispatchTouchCursorEvent(translation, event, *this, [this](QMouseEvent* translated) {
			_mouseReleaseEvent(translated);
		});
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
		auto translation = touch_cursor->mouseDoubleClickEvent(*event);
		dispatchTouchCursorEvent(translation, event, *this, [this](QMouseEvent* translated) {
			_mouseDoubleClickEvent(translated);
		});
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
	const auto vertical_delta = event->angleDelta().y();
	if (vertical_delta != 0)
	{
		if (view)
		{
			auto degrees = vertical_delta / 8.0;
			auto num_steps = degrees / 15.0;
			auto cursor_pos_view = viewportToView(event->position());
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
				QMouseEvent mouse_event {
					QEvent::MouseMove,
					event->position(),
					event->globalPosition(),
					Qt::NoButton,
					QApplication::mouseButtons(),
					event->modifiers(),
					event->pointingDevice()
				};
				mouse_event.setTimestamp(event->timestamp());
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

}  // namespace OpenOrienteering
