/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/qpainter_renderer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <unordered_map>

#include <QBrush>
#include <QByteArray>
#include <QGlyphRun>
#include <QImage>
#include <QPaintEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRawFont>

#include "settings.h"
#include "render/frame_pipeline.h"
#include "render/qpainter_frame_renderer.h"
#include "render/qt_render_bridge.h"

namespace OpenOrienteering::render {

namespace {

void fixPenForPdf(QPen& pen, const QPainter& painter)
{
#ifdef QT_PRINTSUPPORT_LIB
	if (Q_UNLIKELY(painter.paintEngine()->type() == QPaintEngine::Pdf))
	{
		auto const limit = pen.miterLimit();
		pen.setMiterLimit(qSqrt(1.0 + limit * limit * 4));
	}
#else
	Q_UNUSED(pen)
	Q_UNUSED(painter)
#endif
}

Qt::PenCapStyle qtCap(LineCap cap)
{
	switch (cap)
	{
	case LineCap::Flat: return Qt::FlatCap;
	case LineCap::Round: return Qt::RoundCap;
	case LineCap::Square: return Qt::SquareCap;
	}
	Q_UNREACHABLE_RETURN(Qt::FlatCap);
}

Qt::PenJoinStyle qtJoin(LineJoin join)
{
	switch (join)
	{
	case LineJoin::Bevel: return Qt::BevelJoin;
	case LineJoin::Miter: return Qt::MiterJoin;
	case LineJoin::Round: return Qt::RoundJoin;
	}
	Q_UNREACHABLE_RETURN(Qt::MiterJoin);
}

QPainter::CompositionMode qtBlend(BlendMode blend)
{
	switch (blend)
	{
	case BlendMode::SourceOver: return QPainter::CompositionMode_SourceOver;
	case BlendMode::Multiply: return QPainter::CompositionMode_Multiply;
	}
	Q_UNREACHABLE_RETURN(QPainter::CompositionMode_SourceOver);
}

QPen makePen(Color color, const StrokeStyle& style, const QPainter& painter)
{
	QPen pen(toQColor(color), style.width);
	pen.setCapStyle(qtCap(style.cap));
	pen.setJoinStyle(qtJoin(style.join));
	if (style.join == LineJoin::Miter)
	{
		pen.setMiterLimit(style.miter_limit);
		fixPenForPdf(pen, painter);
	}
	return pen;
}

class PathCache
{
public:
	const QPainterPath& get(const PathPtr& path)
	{
		auto const* key = path.get();
		auto const found = paths_.find(key);
		if (found != paths_.end())
			return found->second;
		return paths_.emplace(key, toQPainterPath(*path)).first->second;
	}

private:
	std::unordered_map<const Path*, QPainterPath> paths_;
};

template <typename Draw>
void withQuality(QPainter& painter, QualityHint quality,
	             bool text_antialiasing, bool force_allowed, Draw&& draw)
{
	if (quality == QualityHint::Default)
	{
		draw();
		return;
	}

	if (quality == QualityHint::ForceAntialiasing)
	{
		if (!force_allowed)
		{
			draw();
			return;
		}
		painter.save();
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setRenderHint(QPainter::TextAntialiasing, true);
		draw();
		painter.restore();
		return;
	}

	if (text_antialiasing)
	{
		draw();
		return;
	}
	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, false);
	painter.setRenderHint(QPainter::TextAntialiasing, false);
	draw();
	painter.restore();
}

QPainterPath glyphPath(const GlyphRun& run, const QRawFont& font)
{
	QPainterPath path;
	path.setFillRule(Qt::WindingFill);
	for (auto const& glyph : run.glyphs)
	{
		auto outline = font.pathForGlyph(glyph.id);
		outline.translate(glyph.position.x, glyph.position.y);
		path.addPath(outline);
	}
	return path;
}

void drawLinePattern(QPainter& painter, const QPainterPath& path,
	                 const DrawLinePattern& pattern)
{
	if (pattern.spacing <= 0 || pattern.line_width <= 0)
		return;
	if (pattern.line_width >= pattern.spacing)
	{
		painter.fillPath(path, toQColor(pattern.color));
		return;
	}

	auto canvas = path.controlPointRect();
	auto const margin = pattern.line_width / 2;
	canvas.adjust(-margin, -margin, margin, margin);
	QPen pen(toQColor(pattern.color), pattern.line_width, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);

	painter.save();
	painter.setClipPath(path, Qt::IntersectClip);
	painter.setPen(pen);
	if (std::abs(pattern.angle - M_PI / 2) < 0.0001)
	{
		auto current = pattern.offset
		               + std::ceil((canvas.left() - pattern.offset) / pattern.spacing) * pattern.spacing;
		for (; current < canvas.right(); current += pattern.spacing)
			painter.drawLine(QPointF(current, canvas.top()), QPointF(current, canvas.bottom()));
	}
	else if (std::abs(pattern.angle) < 0.0001)
	{
		auto current = pattern.offset
		               + std::ceil((canvas.top() - pattern.offset) / pattern.spacing) * pattern.spacing;
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
		               + std::ceil((rotated.top() - pattern.offset) / pattern.spacing) * pattern.spacing;
		for (; current < rotated.bottom(); current += pattern.spacing)
			painter.drawLine(QPointF(rotated.left(), current), QPointF(rotated.right(), current));
	}
	painter.restore();
}

}  // namespace

void QPainterRenderer::render(QPainter& painter, const RenderIR& ir,
	                          const RenderRequest& request) const
{
	PathCache paths;
	auto const antialiasing_disabled = request.options.testFlag(RenderConfig::DisableAntialiasing);
	auto text_antialiasing = true;
	if (request.options.testFlag(RenderConfig::Screen))
	{
		text_antialiasing = Settings::getInstance()
		                    .getSettingCached(Settings::MapDisplay_TextAntialiasing)
		                    .toBool();
	}
	painter.save();
	if (antialiasing_disabled)
	{
		painter.setRenderHint(QPainter::Antialiasing, false);
		painter.setRenderHint(QPainter::TextAntialiasing, false);
	}

	for (auto const& command : ir.commands)
	{
		std::visit([&](auto const& op) {
			using T = std::decay_t<decltype(op)>;
			if constexpr (std::is_same_v<T, PushTransform>)
			{
				painter.save();
				painter.setWorldTransform(toQTransform(op.transform), true);
			}
			else if constexpr (std::is_same_v<T, PopTransform>)
			{
				painter.restore();
			}
			else if constexpr (std::is_same_v<T, PushClip>)
			{
				painter.save();
				painter.setClipPath(paths.get(op.path), Qt::IntersectClip);
			}
			else if constexpr (std::is_same_v<T, PopClip>)
			{
				painter.restore();
			}
			else if constexpr (std::is_same_v<T, PushLayer>)
			{
				painter.save();
				painter.setOpacity(painter.opacity() * op.opacity);
				painter.setCompositionMode(qtBlend(op.blend));
			}
			else if constexpr (std::is_same_v<T, PopLayer>)
			{
				painter.restore();
			}
			else if constexpr (std::is_same_v<T, FillPath>)
			{
				withQuality(painter, op.quality, text_antialiasing, !antialiasing_disabled, [&] {
					painter.setPen(Qt::NoPen);
					painter.setBrush(toQColor(op.color));
					painter.drawPath(paths.get(op.path));
				});
			}
			else if constexpr (std::is_same_v<T, StrokePath>)
			{
				withQuality(painter, op.quality, text_antialiasing, !antialiasing_disabled, [&] {
					painter.setPen(makePen(op.color, op.style, painter));
					painter.setBrush(Qt::NoBrush);
					painter.drawPath(paths.get(op.path));
				});
			}
			else if constexpr (std::is_same_v<T, FillEllipse>)
			{
				withQuality(painter, op.quality, text_antialiasing, !antialiasing_disabled, [&] {
					painter.setPen(Qt::NoPen);
					painter.setBrush(toQColor(op.color));
					painter.drawEllipse(toQRectF(op.bounds));
				});
			}
			else if constexpr (std::is_same_v<T, StrokeEllipse>)
			{
				withQuality(painter, op.quality, text_antialiasing, !antialiasing_disabled, [&] {
					painter.setPen(makePen(op.color, op.style, painter));
					painter.setBrush(Qt::NoBrush);
					painter.drawEllipse(toQRectF(op.bounds));
				});
			}
			else if constexpr (std::is_same_v<T, DrawGlyphRun>)
			{
				if (!op.run || !op.run->font || !op.run->font->data)
					return;
				auto const& bytes = *op.run->font->data;
				QByteArray data(reinterpret_cast<const char*>(bytes.data()), qsizetype(bytes.size()));
				QRawFont font(data, op.run->size,
				              op.run->hint ? QFont::PreferDefaultHinting : QFont::PreferNoHinting);
				if (!font.isValid())
					return;
				painter.save();
				painter.setWorldTransform(toQTransform(op.run->transform), true);
				if (op.outline)
				{
					painter.setPen(makePen(op.color, op.stroke, painter));
					painter.setBrush(Qt::NoBrush);
					painter.drawPath(glyphPath(*op.run, font));
				}
				else
				{
					QList<quint32> indexes;
					QList<QPointF> positions;
					indexes.reserve(qsizetype(op.run->glyphs.size()));
					positions.reserve(qsizetype(op.run->glyphs.size()));
					for (auto const& glyph : op.run->glyphs)
					{
						indexes.push_back(glyph.id);
						positions.push_back({ glyph.position.x, glyph.position.y });
					}
					QGlyphRun glyph_run;
					glyph_run.setRawFont(font);
					glyph_run.setGlyphIndexes(indexes);
					glyph_run.setPositions(positions);
					painter.setPen(toQColor(op.color));
					painter.drawGlyphRun({}, glyph_run);
				}
				painter.restore();
			}
			else if constexpr (std::is_same_v<T, DrawImage>)
			{
				if (!op.image || !op.image->rgba8)
					return;
				auto const& bytes = *op.image->rgba8;
				auto const required_size = std::uint64_t(op.image->bytes_per_row) * op.image->height;
				if (op.image->width == 0 || op.image->height == 0
				    || op.image->width > std::uint32_t(std::numeric_limits<int>::max())
				    || op.image->height > std::uint32_t(std::numeric_limits<int>::max())
				    || op.image->bytes_per_row < std::uint64_t(op.image->width) * 4
				    || required_size > bytes.size())
				{
					return;
				}
				QImage image(bytes.data(), int(op.image->width), int(op.image->height),
				             qsizetype(op.image->bytes_per_row), QImage::Format_RGBA8888);
				painter.save();
				painter.setOpacity(painter.opacity() * op.opacity);
				painter.drawImage(toQRectF(op.target), image);
				painter.restore();
			}
			else if constexpr (std::is_same_v<T, DrawLinePattern>)
			{
				drawLinePattern(painter, paths.get(op.outline), op);
			}
		}, command);
	}
	painter.restore();
}

void QPainterRenderer::draw(QPainter& painter, const MapRenderSnapshot& snapshot,
	                        const RenderRequest& request) const
{
	auto const ir = snapshot.buildIR(request);
	render(painter, *ir, request);
}

void QPainterRenderer::drawColorSeparation(QPainter& painter,
	                                       const MapRenderSnapshot& snapshot,
	                                       const RenderRequest& request,
	                                       int separation_priority,
	                                       bool use_color) const
{
	auto const ir = snapshot.buildColorSeparationIR(request, separation_priority, use_color);
	render(painter, *ir, request);
}

void QPainterRenderer::drawOverprintingSimulation(QPainter& painter,
	                                              const MapRenderSnapshot& snapshot,
	                                              const RenderRequest& request) const
{
	auto const* device = painter.device();
	if (!device)
		return;

	FramePlanner planner;
	auto frame = planner.plan(snapshot, {
		{
			std::uint32_t(std::max(0, device->width())),
			std::uint32_t(std::max(0, device->height())),
			std::max(1.0, double(device->devicePixelRatioF())),
			fromQTransform(painter.worldTransform()),
		},
		request,
		true,
	});
	QPainterFrameRenderer().render(painter, *frame);
}

}  // namespace OpenOrienteering::render
