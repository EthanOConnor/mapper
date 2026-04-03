## General

This document is about building OpenOrienteering Mapper from source code.

### Prerequisites

- **CMake** >= 3.28.3 (https://cmake.org/)
- **Ninja** build system (https://ninja-build.org/)
- A C++20 compiler (GCC >= 11, Clang >= 14, MSVC >= 2022)

### Dependencies

- **Qt** >= 5.15 (https://www.qt.io/)
- **PROJ** >= 9.4 (https://proj.org/)
- **GDAL** >= 3.8 (https://gdal.org/) -- optional, enabled by default
- **zlib** (https://zlib.net/)
- **Clipper2** (https://github.com/AngusJohnson/Clipper2) -- built from embedded source


## Quick Start with CMake Presets

The project uses CMake presets for a consistent build experience across
platforms and between local development and CI. Run `cmake --list-presets`
to see all available presets.

### Linux (Ubuntu 24.04)

Install dependencies:
```
sudo apt-get install build-essential ninja-build \
  qtbase5-dev qtbase5-private-dev qttools5-dev qttools5-dev-tools \
  libqt5sensors5-dev libqt5serialport5-dev libqt5sql5-sqlite \
  qtpositioning5-dev qt5-image-formats-plugins \
  libproj-dev libgdal-dev zlib1g-dev \
  libcups2-dev doxygen graphviz
```

Build and test:
```
cmake --preset dev-linux
cmake --build --preset dev-linux
ctest --preset dev-linux
```

### macOS

Install dependencies with Homebrew:
```
brew install qt@5 proj gdal ninja doxygen
```

Build and test:
```
cmake --preset dev-macos -DCMAKE_PREFIX_PATH="$(brew --prefix qt@5)"
cmake --build --preset dev-macos
ctest --preset dev-macos
```

### Windows (MSYS2)

In an MSYS2 MINGW64 terminal, install dependencies:
```
pacman -S \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-qt5-base \
  mingw-w64-x86_64-qt5-tools \
  mingw-w64-x86_64-qt5-location \
  mingw-w64-x86_64-qt5-sensors \
  mingw-w64-x86_64-qt5-serialport \
  mingw-w64-x86_64-qt5-imageformats \
  mingw-w64-x86_64-qt5-translations \
  mingw-w64-x86_64-proj \
  mingw-w64-x86_64-gdal \
  mingw-w64-x86_64-doxygen
```

Build and test:
```
cmake --preset dev-windows
cmake --build --preset dev-windows
ctest --preset dev-windows
```


## Manual CMake Configuration

If you prefer not to use presets:
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```


## CI

CI runs on GitHub Actions using pinned runner images:
- `ubuntu-24.04` for Linux
- `windows-2025` for Windows (MSYS2 MINGW64)
- `macos-15` for macOS

See `.github/workflows/ci.yml` for PR validation and
`.github/workflows/release.yml` for release packaging.


## Packaging

Development presets disable packaging automatically
(`Mapper_DEVELOPMENT_BUILD` suppresses `Mapper_PACKAGE_*` defaults).
CI presets disable it explicitly. Dedicated `release-*` presets enable
dependency bundling for distributable packages:

```
cmake --preset release-linux
cmake --build --preset release-linux
cd build/release-linux && cpack -G DEB
```

Platform-specific outputs:
- DEB on Linux via CPack (system deps, no bundling)
- DragNDrop/DMG on macOS via CPack (bundles Qt, PROJ, GDAL)
- ZIP on Windows by staging the install tree (bundles Qt, PROJ, GDAL)


## Binary Packages and Distribution

Even under open source licenses, distributing and/or using code in source or
binary form creates certain legal obligations, such as the distribution of the
corresponding source code and build instructions for GPL licensed binaries,
and displaying copyright statements and disclaimers.

macOS packages are built using CPack. Windows release artifacts are created by
installing into a staging directory and zipping that tree. Desktop release
artifacts bundle the required 3rd-party components such as Qt binaries and
translations, and PROJ / GDAL binaries and data.
