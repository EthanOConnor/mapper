/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "presentation/vello_canvas.h"

#include <utility>

#include <QtGlobal>
#include <QVBoxLayout>

namespace OpenOrienteering::presentation {

VelloCanvas::VelloCanvas(QWidget* parent)
 : QWidget(parent)
 , surface_(new NativeSurfaceWindow())
 , container_(QWidget::createWindowContainer(surface_, this))
 , renderer_(std::make_unique<render::VelloRenderer>())
{
	setFocusPolicy(Qt::NoFocus);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	container_->setFocusPolicy(Qt::NoFocus);
	container_->setAttribute(Qt::WA_TransparentForMouseEvents);
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(container_);

	surface_->setStateHandler([this](auto const& state) {
		surface_state_ = state;
		if (!renderer_->setSurface(state))
			qFatal("Vello lifecycle queue rejected an ordered native surface state");
		if (state.phase == SurfacePhase::Exposed)
			submitCurrentFrame();
	});
	surface_->setFrameRequestHandler([this] { submitCurrentFrame(); });
}

VelloCanvas::~VelloCanvas()
{
	surface_->setStateHandler({});
	surface_->setFrameRequestHandler({});
	renderer_.reset();
}

void VelloCanvas::setFrame(render::FramePacketPtr frame)
{
	frame_ = std::move(frame);
	submitCurrentFrame();
}

void VelloCanvas::setBackground(render::Color color)
{
	background_ = color;
	submitCurrentFrame();
}

std::optional<render::VelloFrameResult> VelloCanvas::takeResult()
{
	return renderer_->takeResult();
}

std::string VelloCanvas::lastError() const
{
	return renderer_->lastError();
}

const NativeSurfaceState& VelloCanvas::surfaceState() const noexcept
{
	return surface_state_;
}

std::size_t VelloCanvas::encodedSceneCount() const noexcept
{
	return renderer_->encodedSceneCount();
}

void VelloCanvas::submitCurrentFrame()
{
	if (!frame_ || surface_state_.phase != SurfacePhase::Exposed)
		return;
	if (!renderer_->submit(frame_, surface_state_, background_))
		qFatal("Vello latest-wins frame channel rejected a valid frame");
}

}  // namespace OpenOrienteering::presentation
