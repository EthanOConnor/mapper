/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "presentation/native_surface.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <QGuiApplication>
#include <QHideEvent>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QShowEvent>

namespace OpenOrienteering::presentation {

namespace {

template<typename T>
std::uintptr_t opaquePointer(T* pointer)
{
	return reinterpret_cast<std::uintptr_t>(pointer);
}

bool appIsSuspended(Qt::ApplicationState state)
{
	return state == Qt::ApplicationHidden || state == Qt::ApplicationSuspended;
}

}  // namespace

NativeSurfaceDescriptor describeNativeSurface(QWindow& window)
{
	NativeSurfaceDescriptor descriptor;
	auto const platform = QGuiApplication::platformName().toLower();
	descriptor.window = static_cast<std::uintptr_t>(window.winId());

#if defined(Q_OS_MACOS)
	if (platform == QLatin1String("cocoa"))
		descriptor.platform = NativePlatform::AppKit;
#elif defined(Q_OS_IOS)
	if (platform == QLatin1String("ios"))
		descriptor.platform = NativePlatform::UIKit;
#elif defined(Q_OS_WIN)
	if (platform == QLatin1String("windows"))
		descriptor.platform = NativePlatform::Win32;
#elif defined(Q_OS_ANDROID)
	if (platform == QLatin1String("android"))
		descriptor.platform = NativePlatform::AndroidView;
#else
#if QT_CONFIG(wayland)
	if (platform.contains(QLatin1String("wayland")))
	{
		descriptor.platform = NativePlatform::Wayland;
		if (auto* native = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>())
			descriptor.display = opaquePointer(native->display());
		return descriptor;
	}
#endif
#if QT_CONFIG(xcb)
	if (platform == QLatin1String("xcb"))
	{
		descriptor.platform = NativePlatform::Xcb;
		if (auto* native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>())
			descriptor.display = opaquePointer(native->connection());
	}
#endif
#endif

	return descriptor;
}

NativeSurfaceWindow::NativeSurfaceWindow(QWindow* parent)
 : QWindow(parent)
 , suspended_(appIsSuspended(QGuiApplication::applicationState()))
{
#if defined(Q_OS_MACOS) || defined(Q_OS_IOS)
	setSurfaceType(QSurface::MetalSurface);
#elif defined(Q_OS_WIN)
	setSurfaceType(QSurface::Direct3DSurface);
#else
	setSurfaceType(QSurface::VulkanSurface);
#endif
	setFlag(Qt::WindowDoesNotAcceptFocus, true);
	connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
	        [this](Qt::ApplicationState application_state) {
		        suspended_ = appIsSuspended(application_state);
		        publishState();
	        });
	connect(this, &QWindow::screenChanged, this, [this] { publishState(); });
}

NativeSurfaceWindow::~NativeSurfaceWindow()
{
	if (platform_surface_available_)
	{
		platform_surface_available_ = false;
		publishState();
	}
	state_handler_ = {};
	frame_request_handler_ = {};
}

void NativeSurfaceWindow::setStateHandler(StateHandler handler)
{
	state_handler_ = std::move(handler);
	if (state_handler_)
		publishState();
}

void NativeSurfaceWindow::setFrameRequestHandler(FrameRequestHandler handler)
{
	frame_request_handler_ = std::move(handler);
}

const NativeSurfaceState& NativeSurfaceWindow::surfaceState() const noexcept
{
	return state_;
}

void NativeSurfaceWindow::requestFrame()
{
	if (state_.phase == SurfacePhase::Exposed)
		requestUpdate();
}

bool NativeSurfaceWindow::event(QEvent* event)
{
	if (event->type() == QEvent::PlatformSurface)
	{
		auto* surface_event = static_cast<QPlatformSurfaceEvent*>(event);
		if (surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
		{
			platform_surface_available_ = false;
			publishState();
		}
		else if (surface_event->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated)
		{
			platform_surface_available_ = true;
			publishState();
		}
	}
	else if (event->type() == QEvent::DevicePixelRatioChange)
	{
		publishState();
	}
	else if (event->type() == QEvent::UpdateRequest)
	{
		if (state_.phase == SurfacePhase::Exposed && frame_request_handler_)
			frame_request_handler_();
		return true;
	}
	return QWindow::event(event);
}

void NativeSurfaceWindow::exposeEvent(QExposeEvent* event)
{
	QWindow::exposeEvent(event);
	publishState();
}

void NativeSurfaceWindow::resizeEvent(QResizeEvent* event)
{
	QWindow::resizeEvent(event);
	publishState();
}

void NativeSurfaceWindow::showEvent(QShowEvent* event)
{
	QWindow::showEvent(event);
	publishState();
}

void NativeSurfaceWindow::hideEvent(QHideEvent* event)
{
	QWindow::hideEvent(event);
	publishState();
}

void NativeSurfaceWindow::publishState()
{
	NativeSurfaceState next;
	next.logical_width = std::uint32_t(std::max(0, width()));
	next.logical_height = std::uint32_t(std::max(0, height()));
	next.device_pixel_ratio = std::max(1.0, double(devicePixelRatio()));
	next.physical_width = std::uint32_t(std::ceil(next.logical_width * next.device_pixel_ratio));
	next.physical_height = std::uint32_t(std::ceil(next.logical_height * next.device_pixel_ratio));

	if (!platform_surface_available_)
	{
		next.phase = SurfacePhase::Unavailable;
	}
	else
	{
		next.native = describeNativeSurface(*this);
		if (suspended_)
			next.phase = SurfacePhase::Suspended;
		else if (isExposed() && next.physical_width > 0 && next.physical_height > 0)
			next.phase = SurfacePhase::Exposed;
		else
			next.phase = SurfacePhase::Hidden;
	}

	// Handle acquisition can synchronously create a platform surface and thus
	// re-enter this function. Allocate the sequence only after that work so
	// observers always see increasing lifecycle order.
	if (next_sequence_ == std::numeric_limits<std::uint64_t>::max())
		qFatal("Native surface lifecycle sequence space exhausted");
	next.sequence = next_sequence_++;
	state_ = next;
	if (state_handler_)
		state_handler_(state_);
}

}  // namespace OpenOrienteering::presentation
