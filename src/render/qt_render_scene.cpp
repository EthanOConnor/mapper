/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/qt_render_scene.h"

#include <limits>
#include <utility>

#include <QCanvasPath>

namespace OpenOrienteering::render {

namespace {

QCanvasPath compileCanvasPath(const QPainterPath& source)
{
	QCanvasPath canvas(source.elementCount(), source.elementCount() * 2);
	QPointF subpath_start;
	for (int index = 0; index < source.elementCount(); ++index)
	{
		auto const element = source.elementAt(index);
		switch (element.type)
		{
		case QPainterPath::MoveToElement:
			subpath_start = { element.x, element.y };
			canvas.moveTo(subpath_start);
			break;
		case QPainterPath::LineToElement:
			if (QPointF(element.x, element.y) == subpath_start)
				canvas.closePath();
			else
				canvas.lineTo(float(element.x), float(element.y));
			break;
		case QPainterPath::CurveToElement:
			Q_ASSERT(index + 2 < source.elementCount());
			canvas.bezierCurveTo(
				float(element.x), float(element.y),
				float(source.elementAt(index + 1).x), float(source.elementAt(index + 1).y),
				float(source.elementAt(index + 2).x), float(source.elementAt(index + 2).y)
			);
			index += 2;
			break;
		case QPainterPath::CurveToDataElement:
			Q_UNREACHABLE();
		}
	}
	canvas.squeeze();
	return canvas;
}

}  // namespace

class QtRenderPath::Impl
{
public:
	explicit Impl(QPainterPath source)
	 : painter(std::move(source))
	 , canvas(compileCanvasPath(painter))
	{}

	Impl(QPainterPath painter_path, QCanvasPath canvas_path)
	 : painter(std::move(painter_path))
	 , canvas(std::move(canvas_path))
	{}

	QPainterPath painter;
	QCanvasPath canvas;
};

class QtRenderPathBuilder::Impl
{
public:
	explicit Impl(Qt::FillRule fill_rule)
	{
		painter.setFillRule(fill_rule);
	}

	QPainterPath painter;
	QCanvasPath canvas;
};

QtRenderPath::QtRenderPath(QPainterPath path)
 : impl_(std::make_unique<Impl>(std::move(path)))
{}

QtRenderPath::QtRenderPath(QPainterPath painter_path, QCanvasPath canvas_path)
 : impl_(std::make_unique<Impl>(std::move(painter_path), std::move(canvas_path)))
{}

QtRenderPath::~QtRenderPath() = default;

const QPainterPath& QtRenderPath::painterPath() const noexcept
{
	return impl_->painter;
}

const QCanvasPath& QtRenderPath::canvasPath() const noexcept
{
	return impl_->canvas;
}

QtRenderPathBuilder::QtRenderPathBuilder(Qt::FillRule fill_rule)
 : impl_(std::make_unique<Impl>(fill_rule))
{}

QtRenderPathBuilder::~QtRenderPathBuilder() = default;
QtRenderPathBuilder::QtRenderPathBuilder(QtRenderPathBuilder&&) noexcept = default;
QtRenderPathBuilder& QtRenderPathBuilder::operator=(QtRenderPathBuilder&&) noexcept = default;

bool QtRenderPathBuilder::isEmpty() const noexcept
{
	return impl_->canvas.isEmpty();
}

void QtRenderPathBuilder::setFillRule(Qt::FillRule fill_rule)
{
	impl_->painter.setFillRule(fill_rule);
}

void QtRenderPathBuilder::moveTo(QPointF point)
{
	impl_->painter.moveTo(point);
	impl_->canvas.moveTo(point);
}

void QtRenderPathBuilder::lineTo(QPointF point)
{
	impl_->painter.lineTo(point);
	impl_->canvas.lineTo(point);
}

void QtRenderPathBuilder::cubicTo(QPointF control_1, QPointF control_2, QPointF end)
{
	impl_->painter.cubicTo(control_1, control_2, end);
	impl_->canvas.bezierCurveTo(control_1, control_2, end);
}

void QtRenderPathBuilder::closeSubpath()
{
	impl_->painter.closeSubpath();
	impl_->canvas.closePath();
}

void QtRenderPathBuilder::addRect(QRectF rect)
{
	impl_->painter.addRect(rect);
	impl_->canvas.rect(rect);
}

void QtRenderPathBuilder::addEllipse(QRectF rect)
{
	impl_->painter.addEllipse(rect);
	impl_->canvas.ellipse(rect);
}

void QtRenderPathBuilder::connectPath(const QtRenderPathBuilder& other)
{
	if (other.isEmpty())
		return;
	if (isEmpty())
	{
		impl_->painter = other.impl_->painter;
		impl_->canvas = other.impl_->canvas;
		return;
	}

	impl_->painter.connectPath(other.impl_->painter);
	auto const first = other.impl_->painter.elementAt(0);
	impl_->canvas.lineTo(QPointF(first.x, first.y));
	auto const remaining = other.impl_->canvas.commandsSize() - 1;
	if (remaining > 0)
		impl_->canvas.addPath(other.impl_->canvas, 1, remaining);
}

PathPtr QtRenderPathBuilder::finish()
{
	impl_->canvas.squeeze();
	return PathPtr(new QtRenderPath(
		std::move(impl_->painter), std::move(impl_->canvas)
	));
}

PathPtr sharePainterPath(QPainterPath path)
{
	return std::make_shared<const QtRenderPath>(std::move(path));
}

QtRenderSceneBuilder::QtRenderSceneBuilder(Revision revision, QRectF world_bounds)
 : scene_(std::make_shared<QtRenderScene>())
{
	scene_->revision = revision;
	scene_->world_bounds = world_bounds;
}

void QtRenderSceneBuilder::reserve(std::size_t command_capacity,
	                               std::size_t transform_capacity)
{
	scene_->commands.reserve(command_capacity);
	scene_->transforms.reserve(transform_capacity);
}

void QtRenderSceneBuilder::pushTransform(QTransform transform)
{
	if (Q_UNLIKELY(scene_->transforms.size() >= std::numeric_limits<std::uint32_t>::max()))
		qFatal("Scene transform table exhausted");
	auto const index = std::uint32_t(scene_->transforms.size());
	scene_->transforms.push_back(std::move(transform));
	scene_->commands.push_back(PushTransform { index });
}

void QtRenderSceneBuilder::popTransform()
{
	scene_->commands.push_back(PopTransform {});
}

void QtRenderSceneBuilder::pushClip(PathPtr path)
{
	scene_->commands.push_back(PushClip { std::move(path) });
}

void QtRenderSceneBuilder::popClip()
{
	scene_->commands.push_back(PopClip {});
}

void QtRenderSceneBuilder::pushLayer(qreal opacity, QPainter::CompositionMode composition)
{
	scene_->commands.push_back(PushLayer { opacity, composition });
}

void QtRenderSceneBuilder::popLayer()
{
	scene_->commands.push_back(PopLayer {});
}

void QtRenderSceneBuilder::fillPath(PathPtr path, QColor color, QualityHint quality)
{
	scene_->commands.push_back(FillPath { std::move(path), std::move(color), quality });
}

void QtRenderSceneBuilder::strokePath(PathPtr path, QPen pen, QualityHint quality)
{
	scene_->commands.push_back(StrokePath { std::move(path), std::move(pen), quality });
}

void QtRenderSceneBuilder::fillEllipse(QRectF bounds, QColor color, QualityHint quality)
{
	scene_->commands.push_back(FillEllipse { bounds, std::move(color), quality });
}

void QtRenderSceneBuilder::strokeEllipse(QRectF bounds, QPen pen, QualityHint quality)
{
	scene_->commands.push_back(StrokeEllipse { bounds, std::move(pen), quality });
}

void QtRenderSceneBuilder::drawImage(ImagePtr image, QRectF target, qreal opacity)
{
	scene_->commands.push_back(DrawImage { std::move(image), target, opacity });
}

void QtRenderSceneBuilder::drawLinePattern(PathPtr outline, QColor color, qreal angle,
	                                        qreal spacing, qreal offset, qreal line_width)
{
	scene_->commands.push_back(DrawLinePattern {
		std::move(outline), std::move(color), angle, spacing, offset, line_width,
	});
}

void QtRenderSceneBuilder::append(const QtRenderScene& scene)
{
	for (auto const& command : scene.commands)
	{
		if (auto const* transform = std::get_if<PushTransform>(&command))
		{
			Q_ASSERT(transform->transform_index < scene.transforms.size());
			pushTransform(scene.transforms[transform->transform_index]);
		}
		else
		{
			scene_->commands.push_back(command);
		}
	}
}

std::size_t QtRenderSceneBuilder::commandCount() const noexcept
{
	return scene_->commands.size();
}

std::shared_ptr<const QtRenderScene> QtRenderSceneBuilder::finish()
{
	return std::exchange(scene_, std::make_shared<QtRenderScene>());
}

}  // namespace OpenOrienteering::render
