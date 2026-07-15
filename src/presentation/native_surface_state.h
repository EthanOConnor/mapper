/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_NATIVE_SURFACE_STATE_H
#define OPENORIENTEERING_NATIVE_SURFACE_STATE_H

#include <cstdint>

namespace OpenOrienteering::presentation {

enum class NativePlatform : std::uint8_t
{
	Unknown = 0,
	AppKit = 1,
	Win32 = 2,
	AndroidNdk = 3,
	Wayland = 4,
	Xcb = 5,
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
	Unavailable = 0,
	Hidden = 1,
	Exposed = 2,
	Suspended = 3,
};

struct NativeSurfaceState
{
	std::uint64_t sequence = 0;
	SurfacePhase phase = SurfacePhase::Unavailable;
	NativeSurfaceDescriptor native;
	std::uint32_t physical_width = 0;
	std::uint32_t physical_height = 0;
	double device_pixel_ratio = 1;
};

}  // namespace OpenOrienteering::presentation

#endif
