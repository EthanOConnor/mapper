/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/qpainter_frame_renderer.h"

#include <algorithm>

#include <QImage>
#include <QPainter>

#include "render/qpainter_renderer.h"
#include "render/qt_render_bridge.h"

namespace OpenOrienteering::render {

namespace {

QPainter::CompositionMode compositionMode(BlendMode blend)
{
	switch (blend)
	{
	case BlendMode::SourceOver: return QPainter::CompositionMode_SourceOver;
	case BlendMode::Multiply: return QPainter::CompositionMode_Multiply;
	}
	Q_UNREACHABLE_RETURN(QPainter::CompositionMode_SourceOver);
}

void applyOpacity(QImage& image, double opacity)
{
	opacity = std::clamp(opacity, 0.0, 1.0);
	if (opacity == 1)
		return;
	auto* pixel = reinterpret_cast<QRgb*>(image.bits());
	auto const* end = pixel + image.sizeInBytes() / sizeof(QRgb);
	for (; pixel != end; ++pixel)
	{
		*pixel = qRgba(
			int(qRed(*pixel) * opacity),
			int(qGreen(*pixel) * opacity),
			int(qBlue(*pixel) * opacity),
			int(qAlpha(*pixel) * opacity)
		);
	}
}

}  // namespace

FrameCompletion QPainterFrameRenderer::render(QPainter& painter, const FramePacket& frame) const
{
	if (!painter.isActive())
		return { frame.id, FrameStatus::TargetUnavailable };
	auto* image = dynamic_cast<QImage*>(painter.device());
	auto const needs_isolation = std::ranges::any_of(
		frame.vector_passes, &VectorPass::isolated
	);
	if (needs_isolation
	    && (!image || image->format() != QImage::Format_ARGB32_Premultiplied))
		return { frame.id, FrameStatus::TargetUnavailable };

	painter.save();
	QPainterRenderer renderer;
	for (auto const& pass : frame.vector_passes)
	{
		if (!pass.scene)
			continue;
		auto const pass_transform = pass.space == VectorPass::Space::World
		                          ? toQTransform(frame.view.world_to_viewport)
		                          : QTransform{};
		if (pass.isolated)
		{
			QImage layer(image->size(), QImage::Format_ARGB32_Premultiplied);
			layer.setDevicePixelRatio(image->devicePixelRatio());
			layer.fill(Qt::transparent);
			QPainter layer_painter(&layer);
			layer_painter.setRenderHints(painter.renderHints());
			layer_painter.setWorldTransform(pass_transform);
			renderer.render(layer_painter, *pass.scene, true);
			layer_painter.end();
			applyOpacity(layer, pass.opacity);

			painter.save();
			painter.resetTransform();
			painter.setCompositionMode(compositionMode(pass.blend));
			painter.drawImage(QPointF(0, 0), layer);
			painter.restore();
			continue;
		}
		painter.save();
		painter.setWorldTransform(pass_transform, false);
		painter.setCompositionMode(compositionMode(pass.blend));
		painter.setOpacity(painter.opacity() * pass.opacity);
		renderer.render(painter, *pass.scene, true);
		painter.restore();
	}
	painter.restore();
	return { frame.id, FrameStatus::Presented };
}

}  // namespace OpenOrienteering::render
