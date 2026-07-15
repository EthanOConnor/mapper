/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_NATIVE_SURFACE_H
#define OPENORIENTEERING_NATIVE_SURFACE_H

#include <functional>
#include <vector>

#include <QWindow>

#include "presentation/native_surface_state.h"

namespace OpenOrienteering::presentation {

/**
 * Owns the Qt lifecycle of a render-only native surface.
 *
 * It owns no map, snapshot, cache, or renderer. The QWidget canvas remains
 * the input authority when this window is embedded at the screen-renderer
 * cutover.
 */
class NativeSurfaceWindow : public QWindow
{
public:
	using StateHandler = std::function<void(const NativeSurfaceState&)>;
	using InputHandler = std::function<bool(QEvent*)>;

	explicit NativeSurfaceWindow(QWindow* parent = nullptr);
	~NativeSurfaceWindow() override;

	void setStateHandler(StateHandler handler);
	void setInputHandler(InputHandler handler);
	const NativeSurfaceState& surfaceState() const noexcept;
	void refreshState();

protected:
	bool event(QEvent* event) override;
	void exposeEvent(QExposeEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void hideEvent(QHideEvent* event) override;

private:
	void publishState(bool force = false);
#if defined(Q_OS_ANDROID)
	void refreshAndroidNativeWindow(bool retire_if_missing);
	void retireAndroidNativeWindow();
	void scheduleAndroidSurfaceRefresh();
#endif

	StateHandler state_handler_;
	InputHandler input_handler_;
	NativeSurfaceState state_;
	std::uint64_t next_sequence_ = 1;
	bool platform_surface_available_ = false;
	bool suspended_ = false;
#if defined(Q_OS_ANDROID)
	void* android_native_window_ = nullptr;
	std::vector<void*> retired_android_native_windows_;
	bool android_refresh_pending_ = false;
#endif
};

}  // namespace OpenOrienteering::presentation

#endif
