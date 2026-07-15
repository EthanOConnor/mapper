# Building Mapper

Mapper uses one CMake/Ninja build on Linux, macOS, Windows, and Android. The
project baseline is CMake 4.4.0, Ninja 1.13.0, a C++23 compiler, and Qt 6.10.2.
Desktop and Android packages are built by the same presets used in GitHub
Actions.

The Qt baseline is the newest official binary release available for every
supported target. Update the CMake requirement, CI environment, and this guide
together when the next release reaches the complete platform matrix.

## Dependencies

The distributable-build dependency set is declared in `vcpkg.json` and pinned
by its builtin baseline. It currently resolves PROJ 9.8.1, GDAL 3.13.1, and ICU
78.3. Clipper2 2.0.1 and KDSingleApplication 1.2.1 are content-addressed CMake
dependencies. Qt is installed from the official Qt binary repository.

GDAL 3.13.1 temporarily uses `cmake/vcpkg/ports/gdal`; remove that overlay as
soon as the pinned vcpkg baseline provides the same or a newer release.

## Reproducible desktop build

Install Qt 6.10.2 with the Image Formats, Positioning, Sensors, and Serial Port
modules. Clone and bootstrap the vcpkg baseline recorded in `vcpkg.json`,
then expose its root to CMake:

```sh
git clone https://github.com/microsoft/vcpkg.git .vcpkg
git -C .vcpkg checkout "$(python3 -c 'import json; print(json.load(open("vcpkg.json"))["builtin-baseline"])')"
.vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT="$PWD/.vcpkg"
```

On Windows PowerShell, use the same manifest-owned baseline:

```powershell
$baseline = python -c 'import json; print(json.load(open("vcpkg.json"))["builtin-baseline"])'
git -C .vcpkg checkout $baseline
.\.vcpkg\bootstrap-vcpkg.bat -disableMetrics
$env:VCPKG_ROOT = "$PWD\.vcpkg"
```

Select the managed preset for the host:

```sh
cmake --preset dev-macos-vcpkg
cmake --build --preset dev-macos-vcpkg
ctest --preset dev-macos-vcpkg
```

The corresponding presets are `dev-linux-vcpkg` and `dev-windows-vcpkg`.
Release/package configurations are the `ci-linux`, `ci-macos`, and `ci-windows`
presets. For example:

```sh
cmake --preset ci-macos
cmake --build --preset ci-macos
ctest --preset ci-macos
cmake --build --preset ci-macos --target package
```

Package configurations require the managed dependency tree because its notices
are installed with the application. Normal development configurations do not
have that restriction.

## Fast local build with system dependencies

For a short edit/build loop, install current Qt, PROJ, GDAL, ICU, CMake, Ninja,
and Doxygen packages for the host. The dependency versions still must satisfy
the project minimums. Then use the native development preset:

```sh
cmake --preset dev-macos
cmake --build --preset dev-macos
ctest --preset dev-macos
```

Use `dev-linux` or `dev-windows` on the other desktop hosts. If CMake cannot
locate a nonstandard installation, pass its standard package root or
`CMAKE_PREFIX_PATH`; the project has no parallel dependency-discovery system.

## Android

Android targets API 36, has a minimum API of 28, and currently ships arm64-v8a.
Install JDK 21, Android SDK platform/build tools 36, NDK 27.2.12479018, and the
Qt 6.10.2 `android_arm64_v8a` kit. Set `QT_ROOT_DIR`, `VCPKG_ROOT`,
`ANDROID_SDK_ROOT`, and `ANDROID_NDK_ROOT`, then run:

```sh
cmake --preset ci-android
cmake --build --preset ci-android --target apk aab
```

Qt's generated `apk` and `aab` targets own Android packaging; there is no
second platform build system.

## GitHub delivery

Pull requests and `main` pushes build, test, and package every target.
Pushing a `v*` tag builds the same matrix, creates GitHub artifact attestations,
and opens a draft GitHub release containing the desktop, APK, and AAB packages.
The release remains a draft until the club's Apple, Windows, and Android signing
identities are connected and the resulting packages have passed device smoke
tests; unsigned binaries are never published automatically.

## Optional developer targets

Development builds skip the user manual by default. Enable it with
`-DMapper_BUILD_MANUAL=ON`; Doxygen is required. API documentation is outside
the default graph and can be enabled with `-DMapper_BUILD_API_DOCS=ON`, then
built with the `Mapper-api-docs` target.
