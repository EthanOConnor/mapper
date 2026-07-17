/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/qpainter_renderer.h"

#include <cmath>
#include <type_traits>

#include <QPaintEngine>
#include <QPainter>
#include <QPen>

namespace OpenOrienteering::render {

namespace {

QPen targetPen(QPen pen, const QPainter& painter)
{
#ifdef QT_PRINTSUPPORT_LIB
	if (Q_UNLIKELY(painter.paintEngine()->type() == QPaintEngine::Pdf)
	    && pen.joinStyle() == Qt::MiterJoin)
	{
		auto const limit = pen.miterLimit();
		pen.setMiterLimit(qSqrt(1.0 + limit * limit * 4));
	}
#else
	Q_UNUSED(painter)
#endif
	return pen;
}

template <typename Draw>
void withQuality(QPainter& painter, QualityHint quality,
	             bool force_allowed, Draw&& draw)
{
	if (quality == QualityHint::Default || !force_allowed)
	{
		draw();
		return;
	}

	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, true);
	draw();
	painter.restore();
}

void drawLinePattern(QPainter& painter, const DrawLinePattern& pattern)
{
	if (!pattern.outline || pattern.spacing <= 0 || pattern.line_width <= 0)
		return;
	if (pattern.line_width >= pattern.spacing)
	{
		painter.fillPath(pattern.outline->painterPath(), pattern.color);
		return;
	}

	auto canvas = pattern.outline->painterPath().controlPointRect();
	auto const margin = pattern.line_width / 2;
	canvas.adjust(-margin, -margin, margin, margin);
	QPen pen(pattern.color, pattern.line_width, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);

	painter.save();
	painter.setClipPath(pattern.outline->painterPath(), Qt::IntersectClip);
	painter.setPen(pen);
	if (std::abs(pattern.angle - M_PI / 2) < 0.0001)
	{
		auto current = pattern.offset
		               + std::ceil((canvas.left() - pattern.offset) / pattern.spacing)
		                 * pattern.spacing;
		for (; current < canvas.right(); current += pattern.spacing)
			painter.drawLine(QPointF(current, canvas.top()), QPointF(current, canvas.bottom()));
	}
	else if (std::abs(pattern.angle) < 0.0001)
	{
		auto current = pattern.offset
		               + std::ceil((canvas.top() - pattern.offset) / pattern.spacing)
		                 * pattern.spacing;
		for (; current < canvas.bottom(); current += pattern.spacing)
			painter.drawLine(QPointF(canvas.left(), current), QPointF(canvas.right(), current));
	}
	else
	{
		QTransform transform;
		transform.rotateRadians(pattern.angle);
		auto const rotated = transform.inverted().mapRect(canvas);
		painter.setTransform(transform, true);
		auto current = pattern.offset
		               + std::ceil((rotated.top() - pattern.offset) / pattern.spacing)
		                 * pattern.spacing;
		for (; current < rotated.bottom(); current += pattern.spacing)
			painter.drawLine(QPointF(rotated.left(), current), QPointF(rotated.right(), current));
	}
	painter.restore();
}

}  // namespace

void QPainterRenderer::render(QPainter& painter, const QtRenderScene& scene,
	                          bool antialiasing_allowed) const
{
	painter.save();
	if (!antialiasing_allowed)
		painter.setRenderHint(QPainter::Antialiasing, false);

	for (auto const& command : scene.commands)
	{
		std::visit([&](auto const& op) {
			using T = std::decay_t<decltype(op)>;
			if constexpr (std::is_same_v<T, PushTransform>)
			{
				painter.save();
				Q_ASSERT(op.transform_index < scene.transforms.size());
				painter.setWorldTransform(scene.transforms[op.transform_index], true);
			}
			else if constexpr (std::is_same_v<T, PopTransform>)
			{
				painter.restore();
			}
			else if constexpr (std::is_same_v<T, PushClip>)
			{
				painter.save();
				if (op.path)
					painter.setClipPath(op.path->painterPath(), Qt::IntersectClip);
			}
			else if constexpr (std::is_same_v<T, PopClip>)
			{
				painter.restore();
			}
			else if constexpr (std::is_same_v<T, PushLayer>)
			{
				painter.save();
				painter.setOpacity(painter.opacity() * op.opacity);
				painter.setCompositionMode(op.composition);
			}
			else if constexpr (std::is_same_v<T, PopLayer>)
			{
				painter.restore();
			}
			else if constexpr (std::is_same_v<T, FillPath>)
			{
				if (!op.path)
					return;
				withQuality(painter, op.quality, antialiasing_allowed, [&] {
					painter.fillPath(op.path->painterPath(), op.color);
				});
			}
			else if constexpr (std::is_same_v<T, StrokePath>)
			{
				if (!op.path)
					return;
				withQuality(painter, op.quality, antialiasing_allowed, [&] {
					painter.strokePath(op.path->painterPath(), targetPen(op.pen, painter));
				});
			}
			else if constexpr (std::is_same_v<T, FillEllipse>)
			{
				withQuality(painter, op.quality, antialiasing_allowed, [&] {
					painter.setPen(Qt::NoPen);
					painter.setBrush(op.color);
					painter.drawEllipse(op.bounds);
				});
			}
			else if constexpr (std::is_same_v<T, StrokeEllipse>)
			{
				withQuality(painter, op.quality, antialiasing_allowed, [&] {
					painter.setPen(targetPen(op.pen, painter));
					painter.setBrush(Qt::NoBrush);
					painter.drawEllipse(op.bounds);
				});
			}
			else if constexpr (std::is_same_v<T, DrawImage>)
			{
				if (!op.image || op.image->isNull())
					return;
				painter.save();
				painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
				painter.setOpacity(painter.opacity() * op.opacity);
				painter.drawImage(op.target, *op.image);
				painter.restore();
			}
			else if constexpr (std::is_same_v<T, DrawLinePattern>)
			{
				drawLinePattern(painter, op);
			}
		}, command);
	}
	painter.restore();
}

void QPainterRenderer::draw(QPainter& painter, const MapRenderSnapshot& snapshot,
	                        const RenderRequest& request) const
{
	auto const scene = snapshot.buildScene(request);
	render(painter, *scene, !request.options.testFlag(RenderConfig::DisableAntialiasing));
}

void QPainterRenderer::drawColorSeparation(QPainter& painter,
	                                       const MapRenderSnapshot& snapshot,
	                                       const RenderRequest& request,
	                                       int separation_priority,
	                                       bool use_color) const
{
	auto const scene = snapshot.buildColorSeparationScene(
		request, separation_priority, use_color
	);
	render(painter, *scene, !request.options.testFlag(RenderConfig::DisableAntialiasing));
}

}  // namespace OpenOrienteering::render
