/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "presentation/qt_canvas.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QCanvasPainter>
#include <QPainter>

#include "render/qpainter_frame_renderer.h"

namespace OpenOrienteering::presentation {

QtCanvas::QtCanvas(QWidget* parent)
 : QCanvasPainterWidget(parent)
{
	setObjectName(QStringLiteral("mapQtCanvas"));
	setFocusPolicy(Qt::NoFocus);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setFillColor(background_);
	setColorBufferFormat(QRhiWidget::TextureFormat::RGBA8);
	setSampleCount(1);
#ifdef Q_OS_MACOS
	setApi(QRhiWidget::Api::Metal);
#elif defined(Q_OS_WIN)
	setApi(QRhiWidget::Api::Direct3D11);
#endif
}

QtCanvas::~QtCanvas() = default;

void QtCanvas::setFrame(render::FramePacketPtr frame, QColor background)
{
	frame_ = std::move(frame);
	background_ = std::move(background);
	setFillColor(background_);
	update();
}

const render::FramePacketPtr& QtCanvas::currentFrame() const noexcept
{
	return frame_;
}

const std::optional<render::FrameCompletion>& QtCanvas::lastCompletion() const noexcept
{
	return last_completion_;
}

bool QtCanvas::usedSoftwareFallback() const noexcept
{
	return used_software_fallback_;
}

std::size_t QtCanvas::residentImageCount() const noexcept
{
	return renderer_.residentImageCount();
}

void QtCanvas::paint(QCanvasPainter* painter)
{
	if (!painter || !frame_)
		return;

	used_software_fallback_ = render::QtCanvasRenderer::requiresSoftwareFrame(*frame_);
	painter->reset();
	if (used_software_fallback_)
	{
		if (!paintSoftwareFrame(*painter))
			return;
	}
	else
	{
		std::size_t isolated_layer = 0;
		for (auto const& pass : frame_->vector_passes)
		{
			if (!pass.scene)
				continue;
			if (pass.isolated)
				paintIsolatedPass(*painter, pass, isolated_layer++);
			else
				renderer_.renderPass(*painter, *frame_, pass);
		}
		while (layers_.size() > isolated_layer)
		{
			resetLayer(layers_.back(), painter);
			layers_.pop_back();
		}
	}
	last_completion_ = render::FrameCompletion {
		frame_->id, render::FrameStatus::Presented,
	};
}

bool QtCanvas::paintSoftwareFrame(QCanvasPainter& painter)
{
	auto const dpr = std::max(1.0, frame_->view.device_pixel_ratio);
	auto const pixel_size = QSize(
		std::max(1, int(std::ceil(frame_->view.width * dpr))),
		std::max(1, int(std::ceil(frame_->view.height * dpr)))
	);
	if (software_image_.size() != pixel_size
	    || software_image_.format() != QImage::Format_ARGB32_Premultiplied)
	{
		software_image_ = QImage(pixel_size, QImage::Format_ARGB32_Premultiplied);
	}
	software_image_.setDevicePixelRatio(dpr);
	software_image_.fill(background_);
	QPainter image_painter(&software_image_);
	image_painter.setRenderHint(QPainter::Antialiasing, true);
	image_painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
	auto const completion = render::QPainterFrameRenderer().render(image_painter, *frame_);
	image_painter.end();
	if (completion.status != render::FrameStatus::Presented)
	{
		last_completion_ = completion;
		return false;
	}

	if (software_canvas_image_valid_)
		painter.removeImage(software_canvas_image_);
	software_canvas_image_ = painter.addImage(software_image_);
	software_canvas_image_valid_ = true;
	painter.drawImage(
		software_canvas_image_,
		QRectF(0, 0, frame_->view.width, frame_->view.height)
	);
	return true;
}

void QtCanvas::paintIsolatedPass(QCanvasPainter& painter,
	                             const render::VectorPass& pass,
	                             std::size_t layer_index)
{
	auto const dpr = std::max(1.0, frame_->view.device_pixel_ratio);
	auto const pixel_size = QSize(
		std::max(1, int(std::ceil(frame_->view.width * dpr))),
		std::max(1, int(std::ceil(frame_->view.height * dpr)))
	);
	if (layers_.size() <= layer_index)
		layers_.resize(layer_index + 1);
	auto& layer = layers_[layer_index];
	if (layer.pixel_size != pixel_size || layer.canvas.isNull())
	{
		resetLayer(layer, &painter);
		layer.canvas = painter.createCanvas(pixel_size, 1);
		layer.canvas.setFillColor(Qt::transparent);
		layer.image = painter.addImage(layer.canvas);
		layer.pixel_size = pixel_size;
	}

	beginCanvasPainting(layer.canvas);
	painter.reset();
	render::VectorPass content = pass;
	content.opacity = 1;
	content.composition = QPainter::CompositionMode_SourceOver;
	QTransform device_transform;
	device_transform.scale(dpr, dpr);
	renderer_.renderPass(painter, *frame_, content, device_transform);
	endCanvasPainting();

	painter.reset();
	painter.save();
	painter.setGlobalAlpha(float(pass.opacity));
	painter.drawImage(
		layer.image,
		QRectF(0, 0, frame_->view.width, frame_->view.height)
	);
	painter.restore();
}

void QtCanvas::resetLayer(Layer& layer, QCanvasPainter* painter)
{
	if (painter && !layer.image.isNull())
		painter->removeImage(layer.image);
	if (painter && !layer.canvas.isNull())
		painter->destroyCanvas(layer.canvas);
	layer = {};
}

void QtCanvas::graphicsResourcesInvalidated()
{
	renderer_.graphicsResourcesInvalidated();
	layers_.clear();
	software_canvas_image_ = {};
	software_canvas_image_valid_ = false;
	QCanvasPainterWidget::graphicsResourcesInvalidated();
}

}  // namespace OpenOrienteering::presentation
