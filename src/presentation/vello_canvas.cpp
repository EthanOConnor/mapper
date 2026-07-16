/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "presentation/vello_canvas.h"

#include <utility>

#include <QtGlobal>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QTabletEvent>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace OpenOrienteering::presentation {

VelloCanvas::VelloCanvas(QWidget* parent)
 : QWidget(parent)
 , surface_(new NativeSurfaceWindow())
 , container_(QWidget::createWindowContainer(surface_, this))
 , renderer_(std::make_unique<render::VelloRenderer>())
{
	setObjectName(QStringLiteral("mapVelloCanvas"));
	setFocusPolicy(Qt::NoFocus);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	container_->setFocusPolicy(Qt::NoFocus);
	container_->setAttribute(Qt::WA_TransparentForMouseEvents);
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(container_);
	completion_timer_.setInterval(8);
	connect(&completion_timer_, &QTimer::timeout, this, &VelloCanvas::pollResults);
	retry_timer_.setSingleShot(true);
	retry_timer_.setInterval(100);
	// Target creation may fail while a newly exposed platform surface is still
	// settling. Re-publish the lifecycle state so the renderer prepares that
	// native target again; merely resubmitting the frame cannot recover it.
	connect(&retry_timer_, &QTimer::timeout, surface_, &NativeSurfaceWindow::refreshState);

	surface_->setStateHandler([this](auto const& state) {
		surface_state_ = state;
		if (!renderer_->setSurface(state))
			qFatal("Vello lifecycle queue rejected an ordered native surface state");
		if (state.phase == SurfacePhase::Exposed)
			submitCurrentFrame();
		else
			retry_timer_.stop();
	});
	surface_->setInputHandler([this](QEvent* event) { return forwardInputEvent(event); });
}

VelloCanvas::~VelloCanvas()
{
	completion_timer_.stop();
	retry_timer_.stop();
	surface_->setStateHandler({});
	surface_->setInputHandler({});
	renderer_.reset();
}

void VelloCanvas::setFrame(render::FramePacketPtr frame, render::Color background)
{
	frame_ = std::move(frame);
	background_ = background;
	submitCurrentFrame();
}

std::optional<render::VelloFrameResult> VelloCanvas::takeResult()
{
	pollResults();
	if (results_.empty())
		return {};
	auto result = std::move(results_.front());
	results_.pop_front();
	return result;
}

const std::optional<render::VelloFrameResult>& VelloCanvas::lastResult() const noexcept
{
	return last_result_;
}

const render::FramePacketPtr& VelloCanvas::currentFrame() const noexcept
{
	return frame_;
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

void VelloCanvas::setPresentationCursor(const QCursor& cursor)
{
	setCursor(cursor);
	container_->setCursor(cursor);
	surface_->setCursor(cursor);
}

QCursor VelloCanvas::presentationCursor() const
{
	return surface_->cursor();
}

bool VelloCanvas::forwardInputEvent(QEvent* event)
{
	auto* target = parentWidget();
	if (!target || !target->isVisible())
		return false;

	auto const target_origin = QPointF(mapTo(target, QPoint(0, 0)));
	auto const container_origin = QPointF(container_->pos());
	auto const map_to_target = [&](const QPointF& surface_position) {
		return target_origin + container_origin + surface_position;
	};
	auto const deliver = [&](QEvent& forwarded) {
		QCoreApplication::sendEvent(target, &forwarded);
		event->setAccepted(forwarded.isAccepted());
		return true;
	};

	switch (event->type())
	{
	case QEvent::KeyPress:
	case QEvent::KeyRelease:
	{
		auto const* key = static_cast<QKeyEvent*>(event);
		QKeyEvent forwarded(
			key->type(), key->key(), key->modifiers(), key->nativeScanCode(),
			key->nativeVirtualKey(), key->nativeModifiers(), key->text(),
			key->isAutoRepeat(), quint16(key->count()), key->device()
		);
		forwarded.setTimestamp(key->timestamp());
		return deliver(forwarded);
	}

	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonRelease:
	case QEvent::MouseButtonDblClick:
	case QEvent::MouseMove:
	{
		auto const* mouse = static_cast<QMouseEvent*>(event);
		auto const local_position = map_to_target(mouse->position());
		if (event->type() == QEvent::MouseButtonPress
		    && (target->focusPolicy() & Qt::ClickFocus) == Qt::ClickFocus)
			target->setFocus(Qt::MouseFocusReason);
		QMouseEvent forwarded(
			mouse->type(), local_position, local_position, mouse->globalPosition(),
			mouse->button(), mouse->buttons(), mouse->modifiers(), mouse->source(),
			mouse->pointingDevice()
		);
		forwarded.setTimestamp(mouse->timestamp());
		return deliver(forwarded);
	}

	case QEvent::Wheel:
	{
		auto const* wheel = static_cast<QWheelEvent*>(event);
		QWheelEvent forwarded(
			map_to_target(wheel->position()), wheel->globalPosition(),
			wheel->pixelDelta(), wheel->angleDelta(), wheel->buttons(),
			wheel->modifiers(), wheel->phase(), wheel->inverted(), wheel->source(),
			wheel->pointingDevice()
		);
		forwarded.setTimestamp(wheel->timestamp());
		return deliver(forwarded);
	}

	case QEvent::NativeGesture:
	{
		auto const* gesture = static_cast<QNativeGestureEvent*>(event);
		auto const local_position = map_to_target(gesture->position());
		QNativeGestureEvent forwarded(
			gesture->gestureType(), gesture->pointingDevice(), gesture->fingerCount(),
			local_position, local_position, gesture->globalPosition(), gesture->value(),
			gesture->delta()
		);
		forwarded.setTimestamp(gesture->timestamp());
		return deliver(forwarded);
	}

	case QEvent::TabletPress:
	case QEvent::TabletMove:
	case QEvent::TabletRelease:
	{
		auto const* tablet = static_cast<QTabletEvent*>(event);
		QTabletEvent forwarded(
			tablet->type(), tablet->pointingDevice(),
			map_to_target(tablet->position()), tablet->globalPosition(),
			tablet->pressure(), float(tablet->xTilt()), float(tablet->yTilt()),
			float(tablet->tangentialPressure()), tablet->rotation(), float(tablet->z()),
			tablet->modifiers(), tablet->button(), tablet->buttons()
		);
		forwarded.setTimestamp(tablet->timestamp());
		return deliver(forwarded);
	}

	case QEvent::Enter:
	{
		auto const* enter = static_cast<QEnterEvent*>(event);
		auto const local_position = map_to_target(enter->position());
		QEnterEvent forwarded(
			local_position, local_position, enter->globalPosition(),
			enter->pointingDevice()
		);
		forwarded.setTimestamp(enter->timestamp());
		return deliver(forwarded);
	}

	case QEvent::Leave:
	{
		QEvent forwarded(QEvent::Leave);
		return deliver(forwarded);
	}

	case QEvent::TouchBegin:
	case QEvent::TouchUpdate:
	case QEvent::TouchEnd:
	case QEvent::TouchCancel:
		// The canvas covers its input authority exactly, so touch point local
		// coordinates already match. Reusing the public event preserves every
		// point's pressure, history, velocity, and device metadata.
		QCoreApplication::sendEvent(target, event);
		return true;

	default:
		return false;
	}
}

void VelloCanvas::submitCurrentFrame()
{
	if (!frame_ || surface_state_.phase != SurfacePhase::Exposed)
		return;
	if (!renderer_->submit(frame_, surface_state_, background_))
		qFatal("Vello latest-wins frame channel rejected a valid frame");
	completion_timer_.start();
}

void VelloCanvas::pollResults()
{
	while (auto result = renderer_->takeResult())
	{
		last_result_ = *result;
		if (results_.size() == 64)
			results_.pop_front();
		results_.push_back(*result);
		if (result->completion.status == render::FrameStatus::Failed)
			qFatal("Vello frame failed: %s", renderer_->lastError().c_str());
		if (!frame_ || result->completion.frame_id < frame_->id)
			continue;
		switch (result->completion.status)
		{
		case render::FrameStatus::Presented:
			completion_timer_.stop();
			retry_timer_.stop();
			return;
		case render::FrameStatus::TargetUnavailable:
			completion_timer_.stop();
			if (surface_state_.phase == SurfacePhase::Exposed)
				retry_timer_.start();
			return;
		case render::FrameStatus::SurfaceLost:
			completion_timer_.stop();
			retry_timer_.stop();
			surface_->refreshState();
			return;
		case render::FrameStatus::DroppedStale:
		case render::FrameStatus::Failed:
			break;
		}
	}
}

}  // namespace OpenOrienteering::presentation
