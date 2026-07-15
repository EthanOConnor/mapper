/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_QPAINTER_RENDERER_H
#define OPENORIENTEERING_QPAINTER_RENDERER_H

#include "render/render_snapshot.h"

class QPainter;

namespace OpenOrienteering::render {

/** Deterministic reference, print, PDF, and headless interpreter for RenderIR. */
class QPainterRenderer
{
public:
	void render(QPainter& painter, const RenderIR& ir, bool antialiasing_allowed) const;
	void draw(QPainter& painter, const MapRenderSnapshot& snapshot,
	          const RenderRequest& request) const;
	void drawColorSeparation(QPainter& painter, const MapRenderSnapshot& snapshot,
	                         const RenderRequest& request, int separation_priority,
	                         bool use_color) const;
};

}  // namespace OpenOrienteering::render

#endif
