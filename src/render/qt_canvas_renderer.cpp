/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/qt_canvas_renderer.h"

#include <cmath>
#include <iterator>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QCanvasImage>
#include <QCanvasPainter>
#include <QCanvasPath>
#include <QPainterPathStroker>
#include <QtGui/private/qvectorpath_p.h>

namespace OpenOrienteering::render {

namespace {

QCanvasPainter::LineCap canvasCap(Qt::PenCapStyle cap)
{
	switch (cap)
	{
	case Qt::RoundCap: return QCanvasPainter::LineCap::Round;
	case Qt::SquareCap: return QCanvasPainter::LineCap::Square;
	default: return QCanvasPainter::LineCap::Butt;
	}
}

QCanvasPainter::LineJoin canvasJoin(Qt::PenJoinStyle join)
{
	switch (join)
	{
	case Qt::BevelJoin: return QCanvasPainter::LineJoin::Bevel;
	case Qt::RoundJoin: return QCanvasPainter::LineJoin::Round;
	default: return QCanvasPainter::LineJoin::Miter;
	}
}

QCanvasPainter::FillRule canvasFillRule(Qt::FillRule rule)
{
	return rule == Qt::WindingFill ? QCanvasPainter::FillRule::NonZero
	                               : QCanvasPainter::FillRule::EvenOdd;
}

QPainterPath linePatternPath(const DrawLinePattern& pattern)
{
	QPainterPath lines;
	if (!pattern.outline || pattern.spacing <= 0 || pattern.line_width <= 0)
		return lines;

	auto canvas = pattern.outline->painterPath().controlPointRect();
	auto const margin = pattern.line_width / 2;
	canvas.adjust(-margin, -margin, margin, margin);
	if (std::abs(pattern.angle - M_PI / 2) < 0.0001)
	{
		auto current = pattern.offset
		               + std::ceil((canvas.left() - pattern.offset) / pattern.spacing)
		                 * pattern.spacing;
		for (; current < canvas.right(); current += pattern.spacing)
		{
			lines.moveTo(current, canvas.top());
			lines.lineTo(current, canvas.bottom());
		}
	}
	else if (std::abs(pattern.angle) < 0.0001)
	{
		auto current = pattern.offset
		               + std::ceil((canvas.top() - pattern.offset) / pattern.spacing)
		                 * pattern.spacing;
		for (; current < canvas.bottom(); current += pattern.spacing)
		{
			lines.moveTo(canvas.left(), current);
			lines.lineTo(canvas.right(), current);
		}
	}
	else
	{
		QTransform transform;
		transform.rotateRadians(pattern.angle);
		auto const rotated = transform.inverted().mapRect(canvas);
		auto current = pattern.offset
		               + std::ceil((rotated.top() - pattern.offset) / pattern.spacing)
		                 * pattern.spacing;
		for (; current < rotated.bottom(); current += pattern.spacing)
		{
			lines.moveTo(transform.map(QPointF(rotated.left(), current)));
			lines.lineTo(transform.map(QPointF(rotated.right(), current)));
		}
	}
	return lines;
}

QPainterPath clippedLinePattern(const DrawLinePattern& pattern)
{
	auto lines = linePatternPath(pattern);
	if (lines.isEmpty())
		return {};

	QPainterPathStroker stroker;
	stroker.setWidth(pattern.line_width);
	stroker.setCapStyle(Qt::FlatCap);
	stroker.setJoinStyle(Qt::MiterJoin);
	return stroker.createStroke(lines).intersected(pattern.outline->painterPath());
}

QPainterPath dashedStroke(const StrokePath& stroke)
{
	QPainterPathStroker stroker;
	stroker.setWidth(stroke.pen.widthF());
	stroker.setCapStyle(stroke.pen.capStyle());
	stroker.setJoinStyle(stroke.pen.joinStyle());
	stroker.setMiterLimit(stroke.pen.miterLimit());
	stroker.setDashPattern(stroke.pen.dashPattern());
	stroker.setDashOffset(stroke.pen.dashOffset());
	return stroker.createStroke(stroke.path->painterPath());
}

}  // namespace

class QtCanvasRenderer::Impl
{
public:
	struct ImageResource
	{
		std::weak_ptr<const QImage> source;
		QCanvasImage canvas;
	};

	struct SceneResource
	{
		std::weak_ptr<const QtRenderScene> source;
		std::unordered_map<std::size_t, PathPtr> derived_paths;
	};

	struct StencilClip
	{
		PathPtr path;
		QTransform transform;
	};

	const QCanvasImage& image(QCanvasPainter& painter, const ImagePtr& source)
	{
		auto const* key = source.get();
		if (auto found = images.find(key); found != images.end())
			return found->second.canvas;

		auto canvas = painter.addImage(
			*source, QCanvasPainter::ImageFlag::GenerateMipmaps
		);
		return images.emplace(key, ImageResource { source, std::move(canvas) })
		             .first->second.canvas;
	}

	SceneResource& scene(const std::shared_ptr<const QtRenderScene>& source)
	{
		auto const* key = source.get();
		if (auto found = scenes.find(key); found != scenes.end())
			return found->second;
		return scenes.emplace(key, SceneResource { source, {} }).first->second;
	}

	const QtRenderPath& derivedPath(SceneResource& resources,
	                                std::size_t command_index,
	                                QPainterPath source)
	{
		if (auto found = resources.derived_paths.find(command_index);
		    found != resources.derived_paths.end())
		{
			return *found->second;
		}
		auto path = sharePainterPath(std::move(source));
		return *resources.derived_paths.emplace(command_index, std::move(path))
		                 .first->second;
	}

	void restoreStencilClips(QCanvasPainter& painter,
	                         const std::vector<StencilClip>& clips)
	{
		painter.setStencilClip(QList<QRectF>{});
		for (auto const& clip : clips)
		{
			if (!clip.path)
				continue;
			painter.save();
			painter.setTransform(clip.transform);
			painter.setStencilClip(qtVectorPathForPath(clip.path->painterPath()));
			painter.restore();
		}
	}

	void prune(QCanvasPainter& painter)
	{
		for (auto it = scenes.begin(); it != scenes.end(); )
			it = it->second.source.expired() ? scenes.erase(it) : std::next(it);
		for (auto it = images.begin(); it != images.end(); )
		{
			if (!it->second.source.expired())
			{
				++it;
				continue;
			}
			painter.removeImage(it->second.canvas);
			it = images.erase(it);
		}
	}

	void setPen(QCanvasPainter& painter, const QPen& pen)
	{
		painter.setStrokeStyle(pen.color());
		painter.setLineWidth(float(pen.widthF()));
		painter.setLineCap(canvasCap(pen.capStyle()));
		painter.setLineJoin(canvasJoin(pen.joinStyle()));
		// QPen measures the miter limit in full pen widths; Canvas follows the
		// SVG/HTML convention based on half-widths.
		painter.setMiterLimit(float(pen.miterLimit() * 2));
	}

	void renderScene(QCanvasPainter& painter,
	                 const std::shared_ptr<const QtRenderScene>& source,
	                 qreal initial_opacity)
	{
		auto& resources = scene(source);
		std::vector<qreal> alphas { initial_opacity };
		std::vector<StencilClip> stencil_clips;
		painter.save();
		painter.setGlobalAlpha(float(initial_opacity));
		painter.setAntialias(1);
		// QtRenderPath preserves the source contour directions and fill rule.
		painter.setWindingEnforce(false);
		painter.setHighQualityStroking(false);

		for (std::size_t index = 0; index < source->commands.size(); ++index)
		{
			auto const& command = source->commands[index];
			std::visit([&](auto const& op) {
				using T = std::decay_t<decltype(op)>;
				if constexpr (std::is_same_v<T, PushTransform>)
				{
					painter.save();
					alphas.push_back(alphas.back());
					Q_ASSERT(op.transform_index < source->transforms.size());
					painter.transform(source->transforms[op.transform_index]);
				}
				else if constexpr (std::is_same_v<T, PopTransform>)
				{
					painter.restore();
					alphas.pop_back();
				}
				else if constexpr (std::is_same_v<T, PushClip>)
				{
					painter.save();
					alphas.push_back(alphas.back());
					stencil_clips.push_back({ op.path, painter.getTransform() });
					if (op.path)
						painter.setStencilClip(qtVectorPathForPath(op.path->painterPath()));
				}
				else if constexpr (std::is_same_v<T, PopClip>)
				{
					painter.restore();
					alphas.pop_back();
					Q_ASSERT(!stencil_clips.empty());
					auto const removed_active_clip = bool(stencil_clips.back().path);
					stencil_clips.pop_back();
					if (removed_active_clip)
						restoreStencilClips(painter, stencil_clips);
				}
				else if constexpr (std::is_same_v<T, PushLayer>)
				{
					painter.save();
					auto const alpha = alphas.back() * op.opacity;
					alphas.push_back(alpha);
					painter.setGlobalAlpha(float(alpha));
				}
				else if constexpr (std::is_same_v<T, PopLayer>)
				{
					painter.restore();
					alphas.pop_back();
				}
				else if constexpr (std::is_same_v<T, FillPath>)
				{
					if (!op.path)
						return;
					painter.setFillStyle(op.color);
					// Qt 6.12 path groups scale their cached AA fringe with the
					// camera. Direct QCanvasPath submission keeps AA device-sized.
					painter.fill(
						op.path->canvasPath(), canvasFillRule(op.path->painterPath().fillRule())
					);
				}
				else if constexpr (std::is_same_v<T, StrokePath>)
				{
					if (!op.path)
						return;
					if (op.pen.style() != Qt::SolidLine)
					{
						painter.setFillStyle(op.pen.color());
						auto const& path = derivedPath(resources, index, dashedStroke(op));
						painter.fill(
							path.canvasPath(), canvasFillRule(path.painterPath().fillRule())
						);
						return;
					}
					setPen(painter, op.pen);
					// Qt 6.12's cached stroke path expands at the transformed width,
					// then applies the path transform a second time in the vertex
					// shader. Keep immutable QCanvasPath geometry, but let Canvas
					// prepare stroke vertices in device space for correct widths.
					painter.stroke(op.path->canvasPath());
				}
				else if constexpr (std::is_same_v<T, FillEllipse>)
				{
					painter.setFillStyle(op.color);
					painter.beginPath();
					painter.ellipse(op.bounds);
					painter.fill();
				}
				else if constexpr (std::is_same_v<T, StrokeEllipse>)
				{
					setPen(painter, op.pen);
					painter.beginPath();
					painter.ellipse(op.bounds);
					painter.stroke();
				}
				else if constexpr (std::is_same_v<T, DrawImage>)
				{
					if (!op.image || op.image->isNull())
						return;
					painter.save();
					painter.setGlobalAlpha(float(alphas.back() * op.opacity));
					painter.drawImage(image(painter, op.image), op.target);
					painter.restore();
				}
				else if constexpr (std::is_same_v<T, DrawLinePattern>)
				{
					if (!op.outline || op.spacing <= 0 || op.line_width <= 0)
						return;
					if (op.line_width >= op.spacing)
					{
						painter.setFillStyle(op.color);
						painter.fill(
							op.outline->canvasPath(),
							canvasFillRule(op.outline->painterPath().fillRule())
						);
						return;
					}
					painter.setFillStyle(op.color);
					auto const& path = derivedPath(resources, index, clippedLinePattern(op));
					painter.fill(
						path.canvasPath(), canvasFillRule(path.painterPath().fillRule())
					);
				}
			}, command);
		}
		Q_ASSERT(alphas.size() == 1);
		Q_ASSERT(stencil_clips.empty());
		painter.restore();
	}

	std::unordered_map<const QImage*, ImageResource> images;
	std::unordered_map<const QtRenderScene*, SceneResource> scenes;
};

QtCanvasRenderer::QtCanvasRenderer()
 : impl_(std::make_unique<Impl>())
{}

QtCanvasRenderer::~QtCanvasRenderer() = default;

bool QtCanvasRenderer::requiresSoftwareFrame(const FramePacket& frame) noexcept
{
	for (auto const& pass : frame.vector_passes)
	{
		if (pass.composition == QPainter::CompositionMode_Multiply)
			return true;
		if (!pass.scene)
			continue;
		for (auto const& command : pass.scene->commands)
		{
			if (auto const* layer = std::get_if<PushLayer>(&command);
			    layer && layer->composition == QPainter::CompositionMode_Multiply)
			{
				return true;
			}
		}
	}
	return false;
}

void QtCanvasRenderer::renderPass(QCanvasPainter& painter, const FramePacket& frame,
	                              const VectorPass& pass, QTransform device_transform)
{
	if (!pass.scene)
		return;
	impl_->prune(painter);
	painter.save();
	if (!device_transform.isIdentity())
		painter.transform(device_transform);
	if (pass.space == VectorPass::Space::World)
		painter.transform(frame.view.world_to_viewport);
	impl_->renderScene(painter, pass.scene, pass.opacity);
	painter.restore();
}

void QtCanvasRenderer::graphicsResourcesInvalidated() noexcept
{
	impl_ = std::make_unique<Impl>();
}

std::size_t QtCanvasRenderer::residentImageCount() const noexcept
{
	return impl_->images.size();
}

}  // namespace OpenOrienteering::render
