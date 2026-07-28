set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_ARCHITECTURES x86_64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 18.0)
set(VCPKG_OSX_SYSROOT iphonesimulator)
# Distinguish the iOS 18 simulator target from an Intel macOS build host for
# Autoconf packages such as ICU while retaining their Darwin configuration.
set(VCPKG_MAKE_BUILD_TRIPLET "--host=x86_64-apple-darwin24")
set(VCPKG_BUILD_TYPE release)
