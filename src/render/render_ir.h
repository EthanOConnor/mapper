/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    any later version.
 */

#ifndef OPENORIENTEERING_RENDER_IR_H
#define OPENORIENTEERING_RENDER_IR_H

#include <array>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

namespace OpenOrienteering::render {

using Revision = std::uint64_t;

struct Point
{
	double x = 0;
	double y = 0;
};

struct Rect
{
	double x = 0;
	double y = 0;
	double width = -1;
	double height = -1;

	bool isValid() const noexcept;
	bool intersects(const Rect& other) const noexcept;
};

struct Transform
{
	double m11 = 1;
	double m12 = 0;
	double m21 = 0;
	double m22 = 1;
	double dx = 0;
	double dy = 0;
};

/**
 * An sRGB color with unpremultiplied 16-bit components.
 *
 * The explicit representation keeps the render description independent of a
 * particular graphics API while retaining QColor's component precision.
 */
struct Color
{
	std::uint16_t red = 0;
	std::uint16_t green = 0;
	std::uint16_t blue = 0;
	std::uint16_t alpha = 65535;

	Color withAlpha(double opacity) const noexcept;
};

enum class FillRule : std::uint8_t
{
	OddEven,
	Winding,
};

enum class PathVerb : std::uint8_t
{
	MoveTo,
	LineTo,
	CubicTo,
	Close,
};

struct PathElement
{
	PathVerb verb = PathVerb::MoveTo;
	std::array<Point, 3> points {};
};

/** Immutable backend-neutral vector geometry. */
class Path
{
public:
	Path(std::vector<PathElement> elements, FillRule fill_rule, Rect bounds);

	const std::vector<PathElement>& elements() const noexcept;
	FillRule fillRule() const noexcept;
	const Rect& bounds() const noexcept;

private:
	std::vector<PathElement> elements_;
	FillRule fill_rule_;
	Rect bounds_;
};

using PathPtr = std::shared_ptr<const Path>;

class PathBuilder
{
public:
	explicit PathBuilder(FillRule fill_rule = FillRule::OddEven);

	void moveTo(Point point);
	void lineTo(Point point);
	void cubicTo(Point control_1, Point control_2, Point end);
	void close();
	PathPtr finish();

private:
	void include(Point point);

	std::vector<PathElement> elements_;
	FillRule fill_rule_;
	double min_x_ = 0;
	double min_y_ = 0;
	double max_x_ = 0;
	double max_y_ = 0;
	bool has_bounds_ = false;
};

enum class LineCap : std::uint8_t
{
	Flat,
	Round,
	Square,
};

enum class LineJoin : std::uint8_t
{
	Bevel,
	Miter,
	Round,
};

enum class BlendMode : std::uint8_t
{
	SourceOver,
	Multiply,
};

enum class QualityHint : std::uint8_t
{
	Default,
	ForceAntialiasing,
};

struct StrokeStyle
{
	double width = 0;
	LineCap cap = LineCap::Flat;
	LineJoin join = LineJoin::Miter;
	double miter_limit = 4;
	std::vector<double> dash_pattern;
	double dash_offset = 0;
};

struct ImageData
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t bytes_per_row = 0;
	std::shared_ptr<const std::vector<std::uint8_t>> rgba8;
};

struct PushTransform
{
	Transform transform;
};

struct PopTransform {};

struct PushClip
{
	PathPtr path;
};

struct PopClip {};

struct PushLayer
{
	double opacity = 1;
	BlendMode blend = BlendMode::SourceOver;
};

struct PopLayer {};

struct FillPath
{
	PathPtr path;
	Color color;
	QualityHint quality = QualityHint::Default;
};

struct StrokePath
{
	PathPtr path;
	Color color;
	StrokeStyle style;
	QualityHint quality = QualityHint::Default;
};

struct FillEllipse
{
	Rect bounds;
	Color color;
	QualityHint quality = QualityHint::Default;
};

struct StrokeEllipse
{
	Rect bounds;
	Color color;
	StrokeStyle style;
	QualityHint quality = QualityHint::Default;
};

struct DrawImage
{
	std::shared_ptr<const ImageData> image;
	Rect source;
	Transform image_to_scene;
	double opacity = 1;
};

struct DrawLinePattern
{
	PathPtr outline;
	Color color;
	double angle = 0;
	double spacing = 0;
	double offset = 0;
	double line_width = 0;
};

using Command = std::variant<
	PushTransform,
	PopTransform,
	PushClip,
	PopClip,
	PushLayer,
	PopLayer,
	FillPath,
	StrokePath,
	FillEllipse,
	StrokeEllipse,
	DrawImage,
	DrawLinePattern
>;

/** A complete immutable ordered render description. */
class RenderIR
{
public:
	Revision revision = 0;
	Rect world_bounds;
	std::vector<Command> commands;
};

class RenderIRBuilder
{
public:
	explicit RenderIRBuilder(Revision revision = 0, Rect world_bounds = {});

	void pushTransform(Transform transform);
	void popTransform();
	void pushClip(PathPtr path);
	void popClip();
	void pushLayer(double opacity, BlendMode blend = BlendMode::SourceOver);
	void popLayer();
	void fillPath(PathPtr path, Color color,
	              QualityHint quality = QualityHint::Default);
	void strokePath(PathPtr path, Color color, StrokeStyle style,
	                QualityHint quality = QualityHint::Default);
	void fillEllipse(Rect bounds, Color color,
	                 QualityHint quality = QualityHint::Default);
	void strokeEllipse(Rect bounds, Color color, StrokeStyle style,
	                   QualityHint quality = QualityHint::Default);
	void drawImage(std::shared_ptr<const ImageData> image, Rect target,
	               double opacity = 1);
	void drawImage(std::shared_ptr<const ImageData> image, Rect source,
	               Rect target, double opacity = 1);
	void drawImage(std::shared_ptr<const ImageData> image, Rect source,
	               Transform image_to_scene, double opacity = 1);
	void drawLinePattern(PathPtr outline, Color color, double angle,
	                     double spacing, double offset, double line_width);
	void append(const RenderIR& scene);

	std::size_t commandCount() const noexcept;
	std::shared_ptr<const RenderIR> finish();

private:
	std::shared_ptr<RenderIR> ir_;
};

}  // namespace OpenOrienteering::render

#endif
