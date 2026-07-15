/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_NATIVE_SURFACE_H
#define OPENORIENTEERING_NATIVE_SURFACE_H

#include <cstdint>
#include <functional>
#include <vector>

#include <QWindow>

namespace OpenOrienteering::presentation {

enum class NativePlatform : std::uint8_t
{
	Unknown,
	AppKit,
	UIKit,
	Win32,
	AndroidNdk,
	Wayland,
	Xcb,
};

/**
 * Opaque public-Qt handles; platform code gives them their native types.
 * They are valid only while the enclosing state is not Unavailable.
 */
struct NativeSurfaceDescriptor
{
	NativePlatform platform = NativePlatform::Unknown;
	std::uintptr_t window = 0;
	std::uintptr_t display = 0;
};

enum class SurfacePhase : std::uint8_t
{
	Unavailable,
	Hidden,
	Exposed,
	Suspended,
};

struct NativeSurfaceState
{
	std::uint64_t sequence = 0;
	SurfacePhase phase = SurfacePhase::Unavailable;
	NativeSurfaceDescriptor native;
	std::uint32_t logical_width = 0;
	std::uint32_t logical_height = 0;
	std::uint32_t physical_width = 0;
	std::uint32_t physical_height = 0;
	double device_pixel_ratio = 1;
};

NativeSurfaceDescriptor describeNativeSurface(QWindow& window);

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
	using FrameRequestHandler = std::function<void()>;
	using InputHandler = std::function<bool(QEvent*)>;

	explicit NativeSurfaceWindow(QWindow* parent = nullptr);
	~NativeSurfaceWindow() override;

	void setStateHandler(StateHandler handler);
	void setFrameRequestHandler(FrameRequestHandler handler);
	void setInputHandler(InputHandler handler);
	const NativeSurfaceState& surfaceState() const noexcept;
	void requestFrame();
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
	FrameRequestHandler frame_request_handler_;
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
