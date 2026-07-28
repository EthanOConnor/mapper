set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 18.0)
set(VCPKG_OSX_SYSROOT iphoneos)
# vcpkg-make otherwise gives ICU identical unversioned Darwin host/build
# tuples on an arm64 Mac, so configure tries to execute an iPhone binary.
# Darwin 24 is iOS 18's recognized Darwin host tuple and forces cross-builds.
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-apple-darwin24")
set(VCPKG_BUILD_TYPE release)
