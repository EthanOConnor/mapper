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

#ifndef OPENORIENTEERING_QT_RENDER_SCENE_H
#define OPENORIENTEERING_QT_RENDER_SCENE_H

#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>
#include <QTransform>

QT_BEGIN_NAMESPACE
class QCanvasPath;
QT_END_NAMESPACE

namespace OpenOrienteering::render {

using Revision = std::uint64_t;
/**
 * Immutable render geometry with direct representations for both Qt engines.
 *
 * Canvas Painter gets its final QCanvasPath without renderer-side conversion;
 * QPainterPath remains available for print/PDF, software composition, geometry
 * queries, and Qt 6.12's QVectorPath-only arbitrary stencil clip API.
 */
class QtRenderPath
{
public:
	explicit QtRenderPath(QPainterPath path);
	~QtRenderPath();

	QtRenderPath(const QtRenderPath&) = delete;
	QtRenderPath& operator=(const QtRenderPath&) = delete;

	const QPainterPath& painterPath() const noexcept;
	const QCanvasPath& canvasPath() const noexcept;

private:
	QtRenderPath(QPainterPath painter_path, QCanvasPath canvas_path);
	friend class QtRenderPathBuilder;

	class Impl;
	std::unique_ptr<Impl> impl_;
};

using PathPtr = std::shared_ptr<const QtRenderPath>;
using ImagePtr = std::shared_ptr<const QImage>;

/**
 * Builds Canvas and QPainter path representations together at geometry origin.
 *
 * This is the normal path into the live renderer. It performs no conversion:
 * every geometry operation is written directly to both Qt-native consumers.
 */
class QtRenderPathBuilder
{
public:
	explicit QtRenderPathBuilder(Qt::FillRule fill_rule = Qt::OddEvenFill);
	~QtRenderPathBuilder();

	QtRenderPathBuilder(QtRenderPathBuilder&&) noexcept;
	QtRenderPathBuilder& operator=(QtRenderPathBuilder&&) noexcept;
	QtRenderPathBuilder(const QtRenderPathBuilder&) = delete;
	QtRenderPathBuilder& operator=(const QtRenderPathBuilder&) = delete;

	bool isEmpty() const noexcept;
	void setFillRule(Qt::FillRule fill_rule);
	void moveTo(QPointF point);
	void lineTo(QPointF point);
	void cubicTo(QPointF control_1, QPointF control_2, QPointF end);
	void closeSubpath();
	void addRect(QRectF rect);
	void addEllipse(QRectF rect);
	void connectPath(const QtRenderPathBuilder& other);
	PathPtr finish();

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

/**
 * Returns an immutable, identity-stable view of Qt path geometry.
 *
 * This is the compatibility boundary for Qt APIs which only produce a
 * QPainterPath, notably glyph outlines and caller-supplied overlay paths. Map
 * geometry should use QtRenderPathBuilder and avoid conversion altogether.
 */
PathPtr sharePainterPath(QPainterPath path);

enum class QualityHint : std::uint8_t
{
	Default,
	ForceAntialiasing,
};

struct PushTransform
{
	std::uint32_t transform_index = 0;
};

struct PopTransform {};

struct PushClip
{
	PathPtr path;
};

struct PopClip {};

struct PushLayer
{
	qreal opacity = 1;
	QPainter::CompositionMode composition = QPainter::CompositionMode_SourceOver;
};

struct PopLayer {};

struct FillPath
{
	PathPtr path;
	QColor color;
	QualityHint quality = QualityHint::Default;
};

struct StrokePath
{
	PathPtr path;
	QPen pen;
	QualityHint quality = QualityHint::Default;
};

struct FillEllipse
{
	QRectF bounds;
	QColor color;
	QualityHint quality = QualityHint::Default;
};

struct StrokeEllipse
{
	QRectF bounds;
	QPen pen;
	QualityHint quality = QualityHint::Default;
};

struct DrawImage
{
	ImagePtr image;
	QRectF target;
	qreal opacity = 1;
};

struct DrawLinePattern
{
	PathPtr outline;
	QColor color;
	qreal angle = 0;
	qreal spacing = 0;
	qreal offset = 0;
	qreal line_width = 0;
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

/**
 * Complete immutable Qt-native drawing state for one retained contribution.
 *
 * This is a display list rather than a graphics-backend-neutral IR. Its value
 * types are the same Qt paths, images, colors, pens, and transforms consumed
 * by QPainter and consumed directly by Qt Canvas Painter. The list remains
 * necessary because QCanvasPainterWidget owns the render callback and must not
 * read a mutating Map while painting.
 */
class QtRenderScene
{
public:
	Revision revision = 0;
	QRectF world_bounds;
	std::vector<QTransform> transforms;
	std::vector<Command> commands;
};

class QtRenderSceneBuilder
{
public:
	explicit QtRenderSceneBuilder(Revision revision = 0, QRectF world_bounds = {});

	void reserve(std::size_t command_capacity, std::size_t transform_capacity = 0);
	void pushTransform(QTransform transform);
	void popTransform();
	void pushClip(PathPtr path);
	void popClip();
	void pushLayer(
		qreal opacity,
		QPainter::CompositionMode composition = QPainter::CompositionMode_SourceOver
	);
	void popLayer();
	void fillPath(PathPtr path, QColor color,
	              QualityHint quality = QualityHint::Default);
	void strokePath(PathPtr path, QPen pen,
	                QualityHint quality = QualityHint::Default);
	void fillEllipse(QRectF bounds, QColor color,
	                 QualityHint quality = QualityHint::Default);
	void strokeEllipse(QRectF bounds, QPen pen,
	                   QualityHint quality = QualityHint::Default);
	void drawImage(ImagePtr image, QRectF target, qreal opacity = 1);
	void drawLinePattern(PathPtr outline, QColor color, qreal angle,
	                     qreal spacing, qreal offset, qreal line_width);
	void append(const QtRenderScene& scene);

	std::size_t commandCount() const noexcept;
	std::shared_ptr<const QtRenderScene> finish();

private:
	std::shared_ptr<QtRenderScene> scene_;
};

}  // namespace OpenOrienteering::render

#endif
