/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_QT_RENDER_BRIDGE_H
#define OPENORIENTEERING_QT_RENDER_BRIDGE_H

#include "render/render_ir.h"

class QColor;
class QPainterPath;
class QRectF;
class QTransform;

namespace OpenOrienteering::render {

Rect fromQRectF(const QRectF& rect);
QRectF toQRectF(const Rect& rect);

Color fromQColor(const QColor& color);
QColor toQColor(const Color& color);

Transform fromQTransform(const QTransform& transform);
QTransform toQTransform(const Transform& transform);

PathPtr fromQPainterPath(const QPainterPath& path);
QPainterPath toQPainterPath(const Path& path);

}  // namespace OpenOrienteering::render

#endif
