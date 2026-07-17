/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_QT_CANVAS_H
#define OPENORIENTEERING_QT_CANVAS_H

#include <cstddef>
#include <optional>
#include <vector>

#include <QCanvasImage>
#include <QCanvasOffscreenCanvas>
#include <QCanvasPainterWidget>
#include <QColor>
#include <QImage>

#include "render/frame_packet.h"
#include "render/qt_canvas_renderer.h"

namespace OpenOrienteering::presentation {

/** Ordinary Qt Widgets host for Mapper's retained Qt Canvas Painter scene. */
class QtCanvas : public QCanvasPainterWidget
{
Q_OBJECT

public:
	explicit QtCanvas(QWidget* parent = nullptr);
	~QtCanvas() override;

	void setFrame(render::FramePacketPtr frame, QColor background = Qt::white);
	const render::FramePacketPtr& currentFrame() const noexcept;
	const std::optional<render::FrameCompletion>& lastCompletion() const noexcept;
	bool usedSoftwareFallback() const noexcept;
	std::size_t residentImageCount() const noexcept;

protected:
	void paint(QCanvasPainter* painter) override;
	void graphicsResourcesInvalidated() override;

private:
	struct Layer
	{
		QCanvasOffscreenCanvas canvas;
		QCanvasImage image;
		QSize pixel_size;
	};

	bool paintSoftwareFrame(QCanvasPainter& painter);
	void paintIsolatedPass(QCanvasPainter& painter,
	                       const render::VectorPass& pass,
	                       std::size_t layer_index);
	void resetLayer(Layer& layer, QCanvasPainter* painter = nullptr);

	render::QtCanvasRenderer renderer_;
	render::FramePacketPtr frame_;
	QColor background_ = Qt::white;
	std::optional<render::FrameCompletion> last_completion_;
	std::vector<Layer> layers_;
	QImage software_image_;
	QCanvasImage software_canvas_image_;
	bool software_canvas_image_valid_ = false;
	bool used_software_fallback_ = false;
};

}  // namespace OpenOrienteering::presentation

#endif
