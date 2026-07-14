/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_VELLO_CANVAS_H
#define OPENORIENTEERING_VELLO_CANVAS_H

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <QWidget>

#include "presentation/native_surface.h"
#include "render/vello_renderer.h"

namespace OpenOrienteering::presentation {

/**
 * QWidget host for the render-only native Vello surface.
 *
 * The host is transparent to input. It owns no Map or MapView; its caller owns
 * interaction, frame planning, and revision selection.
 */
class VelloCanvas : public QWidget
{
public:
	explicit VelloCanvas(QWidget* parent = nullptr);
	~VelloCanvas() override;

	void setFrame(render::FramePacketPtr frame);
	void setBackground(render::Color color);
	std::optional<render::VelloFrameResult> takeResult();
	std::string lastError() const;
	const NativeSurfaceState& surfaceState() const noexcept;
	std::size_t encodedSceneCount() const noexcept;

private:
	void submitCurrentFrame();

	NativeSurfaceWindow* surface_ = nullptr;
	QWidget* container_ = nullptr;
	std::unique_ptr<render::VelloRenderer> renderer_;
	render::FramePacketPtr frame_;
	render::Color background_ { 65535, 65535, 65535, 65535 };
	NativeSurfaceState surface_state_;
};

}  // namespace OpenOrienteering::presentation

#endif
