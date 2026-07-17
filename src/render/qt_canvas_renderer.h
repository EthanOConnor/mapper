/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_QT_CANVAS_RENDERER_H
#define OPENORIENTEERING_QT_CANVAS_RENDERER_H

#include <cstddef>
#include <memory>

#include "render/frame_packet.h"

QT_BEGIN_NAMESPACE
class QCanvasPainter;
QT_END_NAMESPACE

namespace OpenOrienteering::render {

/** Retained Qt Canvas Painter compiler and command issuer for QtRenderScene. */
class QtCanvasRenderer
{
public:
	QtCanvasRenderer();
	~QtCanvasRenderer();

	QtCanvasRenderer(const QtCanvasRenderer&) = delete;
	QtCanvasRenderer& operator=(const QtCanvasRenderer&) = delete;

	static bool requiresSoftwareFrame(const FramePacket& frame) noexcept;

	/** Draws one non-isolated pass into the painter's current canvas. */
	void renderPass(QCanvasPainter& painter, const FramePacket& frame,
	                const VectorPass& pass, QTransform device_transform = {});

	/** Invalidates handles after the owning Canvas Painter loses its QRhi. */
	void graphicsResourcesInvalidated() noexcept;

	std::size_t residentImageCount() const noexcept;

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace OpenOrienteering::render

#endif
