/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_VELLO_CANVAS_H
#define OPENORIENTEERING_VELLO_CANVAS_H

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QCursor>
#include <QWidget>
#include <QTimer>

#include "presentation/native_surface.h"
#include "render/vello_renderer.h"

namespace OpenOrienteering::presentation {

/**
 * QWidget host for the render-only native Vello surface.
 *
 * It owns no Map or MapView. Native-surface events are forwarded once to the
 * parent QWidget, which remains the input authority and owns all interaction,
 * frame planning, and revision selection.
 */
class VelloCanvas : public QWidget
{
public:
	explicit VelloCanvas(QWidget* parent = nullptr);
	~VelloCanvas() override;

	void setFrame(
		render::FramePacketPtr frame,
		render::Color background = { 65535, 65535, 65535, 65535 }
	);
	std::optional<render::VelloFrameResult> takeResult();
	const std::optional<render::VelloFrameResult>& lastResult() const noexcept;
	const render::FramePacketPtr& currentFrame() const noexcept;
	std::string lastError() const;
	const NativeSurfaceState& surfaceState() const noexcept;
	std::size_t encodedSceneCount() const noexcept;
	void setPresentationCursor(const QCursor& cursor);
	QCursor presentationCursor() const;

private:
	bool forwardInputEvent(QEvent* event);
	void submitCurrentFrame();
	void pollResults();

	NativeSurfaceWindow* surface_ = nullptr;
	QWidget* container_ = nullptr;
	std::unique_ptr<render::VelloRenderer> renderer_;
	render::FramePacketPtr frame_;
	render::Color background_ { 65535, 65535, 65535, 65535 };
	NativeSurfaceState surface_state_;
	QTimer completion_timer_;
	QTimer retry_timer_;
	std::deque<render::VelloFrameResult> results_;
	std::optional<render::VelloFrameResult> last_result_;
	std::vector<std::uint64_t> timing_samples_us_;
	int timing_window_ = 0;
};

}  // namespace OpenOrienteering::presentation

#endif
