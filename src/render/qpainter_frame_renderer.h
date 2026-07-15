/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_QPAINTER_FRAME_RENDERER_H
#define OPENORIENTEERING_QPAINTER_FRAME_RENDERER_H

#include "render/frame_packet.h"

class QPainter;

namespace OpenOrienteering::render {

/** Synchronous reference implementation of the immutable frame contract. */
class QPainterFrameRenderer
{
public:
	FrameCompletion render(QPainter& painter, const FramePacket& frame) const;
};

}  // namespace OpenOrienteering::render

#endif
