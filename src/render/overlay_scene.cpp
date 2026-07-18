/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/overlay_scene.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QColor>
#include <QLineF>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QRect>
#include <QRectF>
#include <QString>

#include "render/qt_render_bridge.h"

namespace OpenOrienteering::render {

namespace {

LineCap lineCap(Qt::PenCapStyle cap)
{
	switch (cap)
	{
	case Qt::RoundCap: return LineCap::Round;
	case Qt::SquareCap: return LineCap::Square;
	default: return LineCap::Flat;
	}
}

LineJoin lineJoin(Qt::PenJoinStyle join)
{
	switch (join)
	{
	case Qt::BevelJoin: return LineJoin::Bevel;
	case Qt::RoundJoin: return LineJoin::Round;
	default: return LineJoin::Miter;
	}
}

std::uint64_t imageKey(std::uint64_t source_key, const QRect& source,
	                   std::uint64_t source_kind)
{
	auto key = source_key ^ source_kind;
	auto mix = [&key](std::uint32_t value) {
		key ^= std::uint64_t(value) + 0x9e3779b97f4a7c15ULL + (key << 6) + (key >> 2);
	};
	mix(std::uint32_t(source.x()));
	mix(std::uint32_t(source.y()));
	mix(std::uint32_t(source.width()));
	mix(std::uint32_t(source.height()));
	return key;
}

}  // namespace

OverlaySceneBuilder::OverlaySceneBuilder()
{
	states_.push_back({});
}

void OverlaySceneBuilder::begin(Revision revision, Rect viewport_bounds)
{
	builder_ = RenderIRBuilder(revision, viewport_bounds);
	states_.clear();
	states_.push_back({});
}

std::shared_ptr<const RenderIR> OverlaySceneBuilder::finish()
{
	if (states_.size() != 1)
		qFatal("Unbalanced transient overlay state");
	return builder_.finish();
}

void OverlaySceneBuilder::save()
{
	states_.push_back(states_.back());
}

void OverlaySceneBuilder::restore()
{
	if (states_.size() <= 1)
		qFatal("Transient overlay restore without save");
	states_.pop_back();
}

void OverlaySceneBuilder::setPen(const QPen& pen) { states_.back().pen = pen; }
void OverlaySceneBuilder::setPen(const QColor& color) { states_.back().pen = QPen(color); }
void OverlaySceneBuilder::setPen(Qt::PenStyle style) { states_.back().pen.setStyle(style); }
void OverlaySceneBuilder::setBrush(const QBrush& brush) { states_.back().brush = brush; }
void OverlaySceneBuilder::setBrush(const QColor& color) { states_.back().brush = QBrush(color); }
void OverlaySceneBuilder::setBrush(Qt::BrushStyle style) { states_.back().brush.setStyle(style); }

void OverlaySceneBuilder::setOpacity(double opacity)
{
	states_.back().opacity = std::clamp(opacity, 0.0, 1.0);
}

double OverlaySceneBuilder::opacity() const noexcept { return states_.back().opacity; }
void OverlaySceneBuilder::setFont(const QFont& font) { states_.back().font = font; }
const QFont& OverlaySceneBuilder::font() const noexcept { return states_.back().font; }

void OverlaySceneBuilder::translate(double dx, double dy) { states_.back().transform.translate(dx, dy); }
void OverlaySceneBuilder::translate(const QPointF& offset) { translate(offset.x(), offset.y()); }
void OverlaySceneBuilder::rotate(double degrees) { states_.back().transform.rotate(degrees); }
void OverlaySceneBuilder::scale(double sx, double sy) { states_.back().transform.scale(sx, sy); }

void OverlaySceneBuilder::setTransform(const QTransform& transform, bool combine)
{
	states_.back().transform = combine ? transform * states_.back().transform : transform;
}

void OverlaySceneBuilder::setWorldTransform(const QTransform& transform, bool combine)
{
	setTransform(transform, combine);
}

void OverlaySceneBuilder::drawLine(const QPointF& start, const QPointF& end)
{
	QPainterPath path(start);
	path.lineTo(end);
	drawShape(path);
}

void OverlaySceneBuilder::drawLine(const QLineF& line) { drawLine(line.p1(), line.p2()); }

void OverlaySceneBuilder::drawPolyline(const QPointF* points, qsizetype count)
{
	if (!points || count < 2)
		return;
	QPainterPath path(points[0]);
	for (qsizetype i = 1; i < count; ++i)
		path.lineTo(points[i]);
	drawShape(path);
}

void OverlaySceneBuilder::drawPolyline(const QPolygonF& points)
{
	drawPolyline(points.constData(), points.size());
}

void OverlaySceneBuilder::drawPolygon(const QPointF* points, qsizetype count)
{
	if (!points || count < 2)
		return;
	QPainterPath path(points[0]);
	for (qsizetype i = 1; i < count; ++i)
		path.lineTo(points[i]);
	path.closeSubpath();
	drawShape(path);
}

void OverlaySceneBuilder::drawPolygon(const QPolygonF& points)
{
	drawPolygon(points.constData(), points.size());
}

void OverlaySceneBuilder::drawRect(const QRectF& rect)
{
	QPainterPath path;
	path.addRect(rect);
	drawShape(path);
}

void OverlaySceneBuilder::drawRect(const QRect& rect) { drawRect(QRectF(rect)); }

void OverlaySceneBuilder::drawRect(int x, int y, int width, int height)
{
	drawRect(QRect(x, y, width, height));
}

void OverlaySceneBuilder::drawEllipse(const QRectF& rect)
{
	QPainterPath path;
	path.addEllipse(rect);
	drawShape(path);
}

void OverlaySceneBuilder::drawEllipse(const QPointF& center, double radius_x, double radius_y)
{
	drawEllipse(QRectF(center.x() - radius_x, center.y() - radius_y,
	                   radius_x * 2, radius_y * 2));
}

void OverlaySceneBuilder::drawPath(const QPainterPath& path) { drawShape(path); }

void OverlaySceneBuilder::fillRect(const QRectF& rect, const QColor& source)
{
	QPainterPath path;
	path.addRect(rect);
	pushStateTransform();
	builder_.fillPath(fromQPainterPath(path), color(source), QualityHint::ForceAntialiasing);
	popStateTransform();
}

void OverlaySceneBuilder::fillRect(const QRect& rect, const QColor& source)
{
	fillRect(QRectF(rect), source);
}

void OverlaySceneBuilder::drawText(const QRect& rect, int flags, const QString& text)
{
	Q_UNUSED(flags)
	auto const lines = text.split(u'\n');
	QFontMetricsF metrics(states_.back().font);
	auto const line_height = metrics.height();
	auto baseline = rect.center().y() - line_height * lines.size() / 2 + metrics.ascent();
	QPainterPath path;
	for (auto const& line : lines)
	{
		auto const x = rect.center().x() - metrics.horizontalAdvance(line) / 2;
		path.addText(x, baseline, states_.back().font, line);
		baseline += line_height;
	}
	if (states_.back().pen.style() == Qt::NoPen)
		return;
	pushStateTransform();
	builder_.fillPath(fromQPainterPath(path), color(states_.back().pen.color()));
	popStateTransform();
}

void OverlaySceneBuilder::drawImage(int x, int y, const QImage& source,
	                                 int source_x, int source_y,
	                                 int source_width, int source_height)
{
	QRect source_rect(source_x, source_y, source_width, source_height);
	auto data = image(source, source_rect,
	                  imageKey(std::uint64_t(source.cacheKey()), source_rect,
	                           0x51494d414745ULL));
	if (!data)
		return;
	pushStateTransform();
	builder_.drawImage(std::move(data), {
		double(x), double(y), double(source_width), double(source_height)
	}, states_.back().opacity);
	popStateTransform();
}

void OverlaySceneBuilder::drawPixmap(const QPointF& top_left, const QPixmap& pixmap)
{
	if (pixmap.isNull())
		return;
	auto const source = pixmap.toImage();
	auto data = image(source, source.rect(),
	                  imageKey(std::uint64_t(pixmap.cacheKey()), source.rect(),
	                           0x515049584d4150ULL));
	if (!data)
		return;
	auto const device_scale = pixmap.devicePixelRatio();
	pushStateTransform();
	builder_.drawImage(std::move(data), {
		top_left.x(), top_left.y(),
		double(source.width()) / device_scale,
		double(source.height()) / device_scale,
	}, states_.back().opacity);
	popStateTransform();
}

void OverlaySceneBuilder::append(const RenderIR& scene)
{
	pushStateTransform();
	if (states_.back().opacity < 1)
		builder_.pushLayer(states_.back().opacity);
	builder_.append(scene);
	if (states_.back().opacity < 1)
		builder_.popLayer();
	popStateTransform();
}

void OverlaySceneBuilder::drawShape(const QPainterPath& path)
{
	if (path.isEmpty())
		return;
	auto immutable_path = fromQPainterPath(path);
	pushStateTransform();
	auto const& state = states_.back();
	auto const brush_color = color(state.brush.color());
	auto hatch = [&](double angle, double spacing = 4, double width = 1) {
		builder_.drawLinePattern(immutable_path, brush_color, angle, spacing, 0, width);
	};
	switch (state.brush.style())
	{
	case Qt::NoBrush:
		break;
	case Qt::SolidPattern:
		builder_.fillPath(immutable_path, color(state.brush.color()),
		                  QualityHint::ForceAntialiasing);
		break;
	case Qt::HorPattern:
		hatch(0);
		break;
	case Qt::VerPattern:
		hatch(M_PI / 2);
		break;
	case Qt::CrossPattern:
		hatch(0);
		hatch(M_PI / 2);
		break;
	case Qt::BDiagPattern:
		hatch(M_PI / 4);
		break;
	case Qt::FDiagPattern:
		hatch(-M_PI / 4);
		break;
	case Qt::DiagCrossPattern:
	case Qt::Dense5Pattern:
		hatch(M_PI / 4);
		hatch(-M_PI / 4);
		break;
	case Qt::Dense1Pattern:
	case Qt::Dense2Pattern:
	case Qt::Dense3Pattern:
	case Qt::Dense4Pattern:
	case Qt::Dense6Pattern:
	case Qt::Dense7Pattern:
	{
		// Approximate Qt's device-space stipples with deterministic retained
		// crosshatching. Dense5 is the only stipple used by Mapper today.
		auto const spacing = 1.5 + 0.75 * (int(state.brush.style()) - int(Qt::Dense1Pattern));
		hatch(M_PI / 4, spacing);
		hatch(-M_PI / 4, spacing);
		break;
	}
	default:
		qFatal("Unsupported transient overlay brush style %d", int(state.brush.style()));
	}
	if (state.pen.style() != Qt::NoPen)
		builder_.strokePath(std::move(immutable_path), color(state.pen.color()), stroke(),
		                    QualityHint::ForceAntialiasing);
	popStateTransform();
}

void OverlaySceneBuilder::pushStateTransform()
{
	builder_.pushTransform(fromQTransform(states_.back().transform));
}

void OverlaySceneBuilder::popStateTransform() { builder_.popTransform(); }

Color OverlaySceneBuilder::color(const QColor& source) const
{
	auto adjusted = source;
	adjusted.setAlphaF(adjusted.alphaF() * states_.back().opacity);
	return fromQColor(adjusted);
}

StrokeStyle OverlaySceneBuilder::stroke() const
{
	auto const& pen = states_.back().pen;
	auto width = pen.widthF();
	if (width <= 0)
		width = 1;
	StrokeStyle style {
		.width = width,
		.cap = lineCap(pen.capStyle()),
		.join = lineJoin(pen.joinStyle()),
		.miter_limit = pen.miterLimit(),
		.dash_pattern = {},
		.dash_offset = 0,
	};
	if (pen.style() != Qt::SolidLine && pen.style() != Qt::NoPen)
	{
		for (auto value : pen.dashPattern())
			style.dash_pattern.push_back(value * width);
		style.dash_offset = pen.dashOffset() * width;
	}
	return style;
}

std::shared_ptr<const ImageData> OverlaySceneBuilder::image(const QImage& source,
	                                                        const QRect& source_rect,
	                                                        std::uint64_t stable_key)
{
	if (source.isNull() || !source.rect().contains(source_rect) || source_rect.isEmpty())
		return {};
	if (auto found = images_.find(stable_key); found != images_.end())
		return found->second;

	auto converted = source.copy(source_rect).convertToFormat(QImage::Format_RGBA8888);
	if (converted.isNull())
		return {};
	auto bytes = std::make_shared<std::vector<std::uint8_t>>();
	auto const row_bytes = std::size_t(converted.width()) * 4;
	bytes->reserve(row_bytes * std::size_t(converted.height()));
	for (int row = 0; row < converted.height(); ++row)
	{
		auto const* begin = converted.constScanLine(row);
		bytes->insert(bytes->end(), begin, begin + row_bytes);
	}
	auto data = std::make_shared<const ImageData>(ImageData {
		std::uint32_t(converted.width()),
			std::uint32_t(converted.height()),
			std::uint32_t(row_bytes),
			std::move(bytes),
			{},
		});
	if (images_.size() >= 128)
		images_.clear();
	images_.emplace(stable_key, data);
	return data;
}

}  // namespace OpenOrienteering::render
