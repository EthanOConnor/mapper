## General

This document is about building OpenOrienteering Mapper from source code.

### Prerequisites

- **CMake** >= 3.28.3 (https://cmake.org/)
- **Ninja** build system (https://ninja-build.org/)
- A C++20 compiler (GCC >= 11, Clang >= 14, MSVC >= 2022)

### Dependencies

- **Qt** >= 6.8 (https://www.qt.io/)
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
  qt6-base-dev qt6-base-private-dev qt6-tools-dev qt6-tools-dev-tools \
  qt6-l10n-tools qt6-sensors-dev qt6-serialport-dev \
  qt6-positioning-dev qt6-image-formats-plugins \
  libproj-dev libgdal-dev zlib1g-dev \
  libcups2-dev doxygen graphviz libegl-dev libgl-dev
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
brew install qt proj gdal ninja doxygen
```

Build and test:
```
cmake --preset dev-macos -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
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
  mingw-w64-x86_64-qt6-base \
  mingw-w64-x86_64-qt6-tools \
  mingw-w64-x86_64-qt6-positioning \
  mingw-w64-x86_64-qt6-sensors \
  mingw-w64-x86_64-qt6-serialport \
  mingw-w64-x86_64-qt6-connectivity \
  mingw-w64-x86_64-qt6-imageformats \
  mingw-w64-x86_64-qt6-translations \
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


## Accelerating Builds with ccache

CI uses [ccache](https://ccache.dev/) automatically. For local development,
create a `CMakeUserPresets.json` (gitignored) that inherits from a dev preset:

```json
{
    "version": 6,
    "configurePresets": [
        {
            "name": "dev",
            "inherits": "dev-macos",
            "cacheVariables": {
                "CMAKE_C_COMPILER_LAUNCHER": "ccache",
                "CMAKE_CXX_COMPILER_LAUNCHER": "ccache"
            }
        }
    ]
}
```

Then use `cmake --preset dev` instead of `cmake --preset dev-macos`.


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
- ZIP on Windows via CPack (bundles Qt, PROJ, GDAL)


## Binary Packages and Distribution

Even under open source licenses, distributing and/or using code in source or
binary form creates certain legal obligations, such as the distribution of the
corresponding source code and build instructions for GPL licensed binaries,
and displaying copyright statements and disclaimers.

Release artifacts for all desktop platforms are built using CPack. They bundle
the required 3rd-party components such as Qt binaries and translations, and
PROJ / GDAL binaries and data.
