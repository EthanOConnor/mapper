/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/render_ir.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace OpenOrienteering::render {

bool Rect::isValid() const noexcept
{
	return width > 0 && height > 0;
}

bool Rect::intersects(const Rect& other) const noexcept
{
	return isValid() && other.isValid()
	       && x < other.x + other.width
	       && other.x < x + width
	       && y < other.y + other.height
	       && other.y < y + height;
}

Color Color::withAlpha(double opacity) const noexcept
{
	auto copy = *this;
	copy.alpha = static_cast<std::uint16_t>(std::clamp(std::lround(opacity * 65535.0), 0l, 65535l));
	return copy;
}

Path::Path(std::vector<PathElement> elements, FillRule fill_rule, Rect bounds)
 : elements_(std::move(elements))
 , fill_rule_(fill_rule)
 , bounds_(bounds)
{
	// nothing
}

const std::vector<PathElement>& Path::elements() const noexcept
{
	return elements_;
}

FillRule Path::fillRule() const noexcept
{
	return fill_rule_;
}

const Rect& Path::bounds() const noexcept
{
	return bounds_;
}

PathBuilder::PathBuilder(FillRule fill_rule)
 : fill_rule_(fill_rule)
{
	// nothing
}

void PathBuilder::include(Point point)
{
	if (!has_bounds_)
	{
		min_x_ = max_x_ = point.x;
		min_y_ = max_y_ = point.y;
		has_bounds_ = true;
		return;
	}
	min_x_ = std::min(min_x_, point.x);
	min_y_ = std::min(min_y_, point.y);
	max_x_ = std::max(max_x_, point.x);
	max_y_ = std::max(max_y_, point.y);
}

void PathBuilder::moveTo(Point point)
{
	elements_.push_back({ PathVerb::MoveTo, { point, {}, {} } });
	include(point);
}

void PathBuilder::lineTo(Point point)
{
	elements_.push_back({ PathVerb::LineTo, { point, {}, {} } });
	include(point);
}

void PathBuilder::cubicTo(Point control_1, Point control_2, Point end)
{
	elements_.push_back({ PathVerb::CubicTo, { control_1, control_2, end } });
	include(control_1);
	include(control_2);
	include(end);
}

void PathBuilder::close()
{
	elements_.push_back({ PathVerb::Close, {} });
}

PathPtr PathBuilder::finish()
{
	Rect bounds;
	if (has_bounds_)
		bounds = { min_x_, min_y_, max_x_ - min_x_, max_y_ - min_y_ };
	return std::make_shared<const Path>(std::move(elements_), fill_rule_, bounds);
}

RenderIRBuilder::RenderIRBuilder(Revision revision, Rect world_bounds)
 : ir_(std::make_shared<RenderIR>())
{
	ir_->revision = revision;
	ir_->world_bounds = world_bounds;
}

void RenderIRBuilder::pushTransform(Transform transform)
{
	ir_->commands.emplace_back(PushTransform { transform });
}

void RenderIRBuilder::popTransform()
{
	ir_->commands.emplace_back(PopTransform {});
}

void RenderIRBuilder::pushClip(PathPtr path)
{
	ir_->commands.emplace_back(PushClip { std::move(path) });
}

void RenderIRBuilder::popClip()
{
	ir_->commands.emplace_back(PopClip {});
}

void RenderIRBuilder::pushLayer(double opacity, BlendMode blend)
{
	ir_->commands.emplace_back(PushLayer { opacity, blend });
}

void RenderIRBuilder::popLayer()
{
	ir_->commands.emplace_back(PopLayer {});
}

void RenderIRBuilder::fillPath(PathPtr path, Color color, ObjectId object_id, QualityHint quality)
{
	ir_->commands.emplace_back(FillPath { std::move(path), color, object_id, quality });
}

void RenderIRBuilder::strokePath(PathPtr path, Color color, StrokeStyle style,
	                              ObjectId object_id, QualityHint quality)
{
	ir_->commands.emplace_back(StrokePath { std::move(path), color, style, object_id, quality });
}

void RenderIRBuilder::fillEllipse(Rect bounds, Color color, ObjectId object_id, QualityHint quality)
{
	ir_->commands.emplace_back(FillEllipse { bounds, color, object_id, quality });
}

void RenderIRBuilder::strokeEllipse(Rect bounds, Color color, StrokeStyle style,
	                                 ObjectId object_id, QualityHint quality)
{
	ir_->commands.emplace_back(StrokeEllipse { bounds, color, style, object_id, quality });
}

void RenderIRBuilder::drawGlyphRun(std::shared_ptr<const GlyphRun> run, Color color,
	                                StrokeStyle stroke, bool outline, ObjectId object_id)
{
	ir_->commands.emplace_back(DrawGlyphRun { std::move(run), color, stroke, outline, object_id });
}

void RenderIRBuilder::drawImage(std::shared_ptr<const ImageData> image, Rect target,
	                             double opacity, ObjectId object_id)
{
	ir_->commands.emplace_back(DrawImage { std::move(image), target, opacity, object_id });
}

void RenderIRBuilder::drawLinePattern(PathPtr outline, Color color, double angle,
	                                   double spacing, double offset, double line_width,
	                                   ObjectId object_id)
{
	ir_->commands.emplace_back(DrawLinePattern {
		std::move(outline), color, angle, spacing, offset, line_width, object_id
	});
}

std::size_t RenderIRBuilder::commandCount() const noexcept
{
	return ir_->commands.size();
}

std::shared_ptr<const RenderIR> RenderIRBuilder::finish()
{
	return std::exchange(ir_, std::make_shared<RenderIR>());
}

}  // namespace OpenOrienteering::render
