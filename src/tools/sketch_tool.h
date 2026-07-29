/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_SKETCH_TOOL_H
#define OPENORIENTEERING_SKETCH_TOOL_H

#include <functional>
#include <vector>

#include <QColor>
#include <QPoint>
#include <QPointer>
#include <QRectF>

#include "core/map_coord.h"
#include "core/sketch_layer.h"
#include "tools/tool.h"

class QAction;
class QCursor;
class QKeyEvent;
class QMouseEvent;

namespace OpenOrienteering {

class ActionGridBar;
class MapEditorController;
class MapPart;
class MapWidget;

/** Interactive pen and stroke eraser for the seamless vector sketch layer. */
class SketchTool final : public MapEditorTool
{
	Q_OBJECT

public:
	SketchTool(MapEditorController* editor, QAction* tool_action);
	~SketchTool() override;

	void setLayer(MapPart* layer);
	void setChooseLayerCallback(std::function<void()> callback);

	void init() override;
	const QCursor& getCursor() const override;

	bool mousePressEvent(QMouseEvent* event, const MapCoordF& map_coord,
	                     MapWidget* widget) override;
	bool mouseMoveEvent(QMouseEvent* event, const MapCoordF& map_coord,
	                    MapWidget* widget) override;
	bool mouseReleaseEvent(QMouseEvent* event, const MapCoordF& map_coord,
	                       MapWidget* widget) override;
	bool keyPressEvent(QKeyEvent* event) override;

	void draw(render::OverlaySceneBuilder* painter, MapWidget* widget) override;

private:
	ActionGridBar* makeToolBar();
	void setColor(const QColor& color);
	void setWidth(SketchLayer::Width width);
	void finishStroke(MapWidget* widget);
	void eraseTouchedStrokes();
	void updateDrawingBounds(MapWidget* widget);

	MapPart* layer = nullptr;
	std::function<void()> choose_layer_callback;
	QPointer<ActionGridBar> widget;
	QAction* width_action = nullptr;

	QColor stroke_color = Qt::black;
	SketchLayer::Width stroke_width = SketchLayer::Width::Medium;
	bool explicit_erasing = false;
	bool stroke_erasing = false;
	bool dragging = false;
	QRectF map_bbox;
	std::vector<MapCoordF> coords;
	QPoint last_sample_pos;

	static constexpr qreal eraser_radius_mm = 1.5;

	Q_DISABLE_COPY(SketchTool)
};

}  // namespace OpenOrienteering

#endif
