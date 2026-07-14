/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_OVERLAY_SCENE_H
#define OPENORIENTEERING_OVERLAY_SCENE_H

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <QBrush>
#include <QFont>
#include <QImage>
#include <QPen>
#include <QTransform>

#include "render/render_ir.h"

class QLineF;
class QPainterPath;
class QPixmap;
class QPolygonF;
class QPointF;
class QRect;
class QRectF;
class QString;

namespace OpenOrienteering::render {

/**
 * Records transient editor graphics directly into backend-neutral RenderIR.
 *
 * Qt value types remain the natural GUI-side geometry vocabulary, but no
 * paint device or QPainter operation crosses this boundary. The builder is
 * retained by MapWidget so small UI images keep stable GPU identities.
 */
class OverlaySceneBuilder
{
public:
	OverlaySceneBuilder();

	void begin(Revision revision, Rect viewport_bounds);
	std::shared_ptr<const RenderIR> finish();

	void save();
	void restore();
	void setPen(const QPen& pen);
	void setPen(const QColor& color);
	void setPen(Qt::PenStyle style);
	void setBrush(const QBrush& brush);
	void setBrush(const QColor& color);
	void setBrush(Qt::BrushStyle style);
	void setOpacity(double opacity);
	double opacity() const noexcept;
	void setFont(const QFont& font);
	const QFont& font() const noexcept;

	void translate(double dx, double dy);
	void translate(const QPointF& offset);
	void rotate(double degrees);
	void scale(double sx, double sy);
	void setTransform(const QTransform& transform, bool combine = false);
	void setWorldTransform(const QTransform& transform, bool combine = false);

	void drawLine(const QPointF& start, const QPointF& end);
	void drawLine(const QLineF& line);
	void drawPolyline(const QPointF* points, qsizetype count);
	void drawPolyline(const QPolygonF& points);
	void drawPolygon(const QPointF* points, qsizetype count);
	void drawPolygon(const QPolygonF& points);
	void drawRect(const QRectF& rect);
	void drawRect(const QRect& rect);
	void drawRect(int x, int y, int width, int height);
	void drawEllipse(const QRectF& rect);
	void drawEllipse(const QPointF& center, double radius_x, double radius_y);
	void drawPath(const QPainterPath& path);
	void fillRect(const QRectF& rect, const QColor& color);
	void fillRect(const QRect& rect, const QColor& color);
	void drawText(const QRect& rect, int flags, const QString& text);

	void drawImage(int x, int y, const QImage& image,
	               int source_x, int source_y, int source_width, int source_height);
	void drawPixmap(const QPointF& top_left, const QPixmap& pixmap);

	void append(const RenderIR& scene);

private:
	struct State
	{
		QPen pen;
		QBrush brush = Qt::NoBrush;
		QFont font;
		double opacity = 1;
		QTransform transform;
	};

	void drawShape(const QPainterPath& path);
	void pushStateTransform();
	void popStateTransform();
	Color color(const QColor& source) const;
	StrokeStyle stroke() const;
	std::shared_ptr<const ImageData> image(const QImage& source, const QRect& source_rect,
	                                       std::uint64_t stable_key);

	RenderIRBuilder builder_;
	std::vector<State> states_;
	std::unordered_map<std::uint64_t, std::shared_ptr<const ImageData>> images_;
};

}  // namespace OpenOrienteering::render

#endif
