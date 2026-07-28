set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
# A mobile parent configure otherwise leaks its iPhone sysroot into host tools
# during Xcode's automatic ZERO_CHECK reconfigure.
set(VCPKG_OSX_SYSROOT macosx)
unset(ENV{IPHONEOS_DEPLOYMENT_TARGET})
unset(ENV{TVOS_DEPLOYMENT_TARGET})
unset(ENV{WATCHOS_DEPLOYMENT_TARGET})
unset(ENV{XROS_DEPLOYMENT_TARGET})
set(VCPKG_BUILD_TYPE release)
