#!/usr/bin/env bash
# build-mobile-deps.sh — Cross-compile native C/C++ dependencies for mobile
#
# Copyright 2024-2026 OpenOrienteering contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# Builds SQLite, libtiff, PROJ, and GDAL as static libraries for Android/iOS.

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

SQLITE_VERSION="${SQLITE_VERSION:-3.51.3}"
TIFF_VERSION="${TIFF_VERSION:-4.7.0}"
PROJ_VERSION="${PROJ_VERSION:-9.6.2}"
GDAL_VERSION="${GDAL_VERSION:-3.11.0}"

# SQLite version encoding: 3.48.0 -> 3480000
sqlite_version_encoded() {
    local IFS='.'
    local parts=($1)
    printf "%d%02d%02d00" "${parts[0]}" "${parts[1]}" "${parts[2]}"
}

SQLITE_SHA256="${SQLITE_SHA256:-acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4}"
TIFF_SHA256="${TIFF_SHA256:-67160e3457365ab96c5b3286a0903aa6e78bdc44c4bc737d2e486bcecb6ba976}"
PROJ_SHA256="${PROJ_SHA256:-53d0cafaee3bb2390264a38668ed31d90787de05e71378ad7a8f35bb34c575d1}"
GDAL_SHA256="${GDAL_SHA256:-758d9c2e83e98da2bec3bdfb3666588e1e98d231d8c02ebac359e36af2caa7a7}"

TARGET=""
PREFIX=""
NDK="${ANDROID_NDK_ROOT:-}"
JOBS=""
CLEAN=0
WITH_GDAL=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOWNLOAD_DIR="${SCRIPT_DIR}/../downloads"

# ---------------------------------------------------------------------------
# Colors
# ---------------------------------------------------------------------------

if [[ -t 1 ]]; then
    C_RED='\033[0;31m'
    C_GREEN='\033[0;32m'
    C_YELLOW='\033[0;33m'
    C_BLUE='\033[0;34m'
    C_BOLD='\033[1m'
    C_RESET='\033[0m'
else
    C_RED='' C_GREEN='' C_YELLOW='' C_BLUE='' C_BOLD='' C_RESET=''
fi

header()  { echo -e "\n${C_BOLD}${C_BLUE}==> $*${C_RESET}"; }
info()    { echo -e "${C_GREEN}    $*${C_RESET}"; }
warn()    { echo -e "${C_YELLOW}    WARNING: $*${C_RESET}"; }
die()     { echo -e "${C_RED}FATAL: $*${C_RESET}" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------

usage() {
    cat <<'EOF'
Usage: build-mobile-deps.sh --target TARGET --prefix DIR [OPTIONS]

Cross-compile SQLite, libtiff, PROJ, and GDAL for mobile platforms.

Targets:
  android-arm64       Android arm64-v8a (requires NDK)
  android-x86_64      Android x86_64 (requires NDK)
  ios-arm64           iOS arm64 (device)
  ios-simulator       iOS simulator (arm64;x86_64 fat)

Options:
  --target TARGET     Target platform (required)
  --prefix DIR        Install prefix (required)
  --ndk PATH          Android NDK path (default: $ANDROID_NDK_ROOT)
  --jobs N            Parallel build jobs (default: auto-detect)
  --clean             Remove build dirs before building
  --no-gdal           Skip libtiff and GDAL (build only SQLite + PROJ)
  -h, --help          Show this help
EOF
    exit 0
}

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)  TARGET="$2"; shift 2 ;;
        --prefix)  PREFIX="$2"; shift 2 ;;
        --ndk)     NDK="$2";    shift 2 ;;
        --jobs)    JOBS="$2";   shift 2 ;;
        --clean)   CLEAN=1;     shift ;;
        --no-gdal) WITH_GDAL=0; shift ;;
        -h|--help) usage ;;
        *) die "Unknown option: $1" ;;
    esac
done

[[ -n "$TARGET" ]] || die "Missing --target"
[[ -n "$PREFIX" ]] || die "Missing --prefix"

# Auto-detect job count
if [[ -z "$JOBS" ]]; then
    if command -v nproc &>/dev/null; then
        JOBS="$(nproc)"
    elif command -v sysctl &>/dev/null; then
        JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    else
        JOBS=4
    fi
fi

PREFIX="$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)"
BUILD_DIR="${PREFIX}/_build"
mkdir -p "$DOWNLOAD_DIR" "$BUILD_DIR"

# ---------------------------------------------------------------------------
# Toolchain setup
# ---------------------------------------------------------------------------

declare -a CMAKE_TOOLCHAIN_ARGS=()

setup_android_toolchain() {
    local abi="$1"
    [[ -n "$NDK" ]] || die "Android target requires --ndk or \$ANDROID_NDK_ROOT"
    [[ -d "$NDK" ]] || die "NDK not found at: $NDK"
    local tc="$NDK/build/cmake/android.toolchain.cmake"
    [[ -f "$tc" ]] || die "NDK toolchain not found: $tc"
    CMAKE_TOOLCHAIN_ARGS=(
        "-DCMAKE_TOOLCHAIN_FILE=$tc"
        "-DANDROID_ABI=$abi"
        "-DANDROID_PLATFORM=android-28"
    )
}

setup_ios_toolchain() {
    CMAKE_TOOLCHAIN_ARGS=(
        "-DCMAKE_SYSTEM_NAME=iOS"
        "-DCMAKE_OSX_ARCHITECTURES=arm64"
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=16.0"
    )
}

setup_ios_simulator_toolchain() {
    CMAKE_TOOLCHAIN_ARGS=(
        "-DCMAKE_SYSTEM_NAME=iOS"
        "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=16.0"
        "-DCMAKE_OSX_SYSROOT=iphonesimulator"
    )
}

case "$TARGET" in
    android-arm64)   setup_android_toolchain "arm64-v8a" ;;
    android-x86_64)  setup_android_toolchain "x86_64" ;;
    ios-arm64)       setup_ios_toolchain ;;
    ios-simulator)   setup_ios_simulator_toolchain ;;
    *) die "Unknown target: $TARGET (expected android-arm64, android-x86_64, ios-arm64, ios-simulator)" ;;
esac

header "Build configuration"
info "Target:    $TARGET"
info "Prefix:    $PREFIX"
info "Build dir: $BUILD_DIR"
info "Jobs:      $JOBS"
info "Versions:  SQLite $SQLITE_VERSION, libtiff $TIFF_VERSION, PROJ $PROJ_VERSION, GDAL $GDAL_VERSION"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

download() {
    local url="$1" dest="$2" expected_sha="$3"
    if [[ -f "$dest" ]]; then
        local actual_sha
        actual_sha="$(shasum -a 256 "$dest" | cut -d' ' -f1)"
        if [[ "$actual_sha" == "$expected_sha" ]]; then
            info "Cached: $(basename "$dest")"
            return 0
        fi
        warn "Checksum mismatch for cached $(basename "$dest"), re-downloading"
        rm -f "$dest"
    fi
    info "Downloading $(basename "$dest") ..."
    curl -fSL --retry 3 -o "$dest" "$url"
    local actual_sha
    actual_sha="$(shasum -a 256 "$dest" | cut -d' ' -f1)"
    if [[ "$actual_sha" != "$expected_sha" ]]; then
        die "SHA256 mismatch for $(basename "$dest")\n  expected: $expected_sha\n  got:      $actual_sha"
    fi
    info "Verified: $(basename "$dest")"
}

cmake_build() {
    local src="$1"; shift
    cmake -S "$src" -B "$src/_build" \
        "${CMAKE_TOOLCHAIN_ARGS[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        "$@"
    cmake --build "$src/_build" --parallel "$JOBS"
    cmake --install "$src/_build"
}

clean_build_dir() {
    local dir="$1"
    if [[ "$CLEAN" -eq 1 && -d "$dir" ]]; then
        info "Cleaning $dir"
        rm -rf "$dir"
    fi
}

# ---------------------------------------------------------------------------
# 1. SQLite
# ---------------------------------------------------------------------------

build_sqlite() {
    header "Building SQLite $SQLITE_VERSION"

    local encoded
    encoded="$(sqlite_version_encoded "$SQLITE_VERSION")"
    local tarball="sqlite-amalgamation-${encoded}.zip"
    local url="https://www.sqlite.org/2026/${tarball}"

    download "$url" "$DOWNLOAD_DIR/$tarball" "$SQLITE_SHA256"

    local src="$BUILD_DIR/sqlite"
    clean_build_dir "$src"
    mkdir -p "$src"
    unzip -qo "$DOWNLOAD_DIR/$tarball" -d "$src"

    # The amalgamation extracts to a subdirectory
    local amalg_dir="$src/sqlite-amalgamation-${encoded}"
    [[ -d "$amalg_dir" ]] || die "Expected directory: $amalg_dir"

    # Generate a minimal CMakeLists.txt for cross-compilation
    cat > "$amalg_dir/CMakeLists.txt" <<'CMAKE_EOF'
cmake_minimum_required(VERSION 3.16)
project(SQLite3 C)

add_library(sqlite3 STATIC sqlite3.c)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
)
target_include_directories(sqlite3 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

install(TARGETS sqlite3 ARCHIVE DESTINATION lib)
install(FILES sqlite3.h DESTINATION include)

# Provide a pkg-config-style cmake config so PROJ can find us
include(CMakePackageConfigHelpers)
install(TARGETS sqlite3 EXPORT SQLite3Targets)
install(EXPORT SQLite3Targets
    FILE SQLite3Targets.cmake
    NAMESPACE SQLite3::
    DESTINATION lib/cmake/SQLite3
)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/SQLite3Config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/SQLite3Config.cmake"
    INSTALL_DESTINATION lib/cmake/SQLite3
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/SQLite3Config.cmake"
    DESTINATION lib/cmake/SQLite3
)
CMAKE_EOF

    cat > "$amalg_dir/SQLite3Config.cmake.in" <<'CONFIG_EOF'
@PACKAGE_INIT@
include("${CMAKE_CURRENT_LIST_DIR}/SQLite3Targets.cmake")
set(SQLite3_FOUND TRUE)
set(SQLite3_INCLUDE_DIRS "${PACKAGE_PREFIX_DIR}/include")
set(SQLite3_LIBRARIES SQLite3::sqlite3)
# PROJ's installed config references SQLite::SQLite3 (CMake's FindSQLite3 target name)
if(NOT TARGET SQLite::SQLite3)
    add_library(SQLite::SQLite3 ALIAS SQLite3::sqlite3)
endif()
CONFIG_EOF

    cmake_build "$amalg_dir"
    info "SQLite installed to $PREFIX"
}

# ---------------------------------------------------------------------------
# 2. libtiff
# ---------------------------------------------------------------------------

build_tiff() {
    header "Building libtiff $TIFF_VERSION"

    local tarball="tiff-${TIFF_VERSION}.tar.gz"
    local url="https://download.osgeo.org/libtiff/${tarball}"

    download "$url" "$DOWNLOAD_DIR/$tarball" "$TIFF_SHA256"

    local src="$BUILD_DIR/tiff"
    clean_build_dir "$src"
    mkdir -p "$src"
    tar -xzf "$DOWNLOAD_DIR/$tarball" -C "$src" --strip-components=1

    cmake_build "$src" \
        -DBUILD_SHARED_LIBS=OFF \
        -Dtiff-tools=OFF \
        -Dtiff-tests=OFF \
        -Dtiff-contrib=OFF \
        -Dtiff-docs=OFF \
        -Djpeg=OFF \
        -Dlzma=OFF \
        -Dzstd=OFF \
        -Dwebp=OFF \
        -Djbig=OFF \
        -Dlerc=OFF

    info "libtiff installed to $PREFIX"
}

# ---------------------------------------------------------------------------
# 3. PROJ
# ---------------------------------------------------------------------------

build_proj() {
    header "Building PROJ $PROJ_VERSION"

    local tarball="proj-${PROJ_VERSION}.tar.gz"
    local url="https://download.osgeo.org/proj/${tarball}"

    download "$url" "$DOWNLOAD_DIR/$tarball" "$PROJ_SHA256"

    local src="$BUILD_DIR/proj"
    clean_build_dir "$src"
    mkdir -p "$src"
    tar -xzf "$DOWNLOAD_DIR/$tarball" -C "$src" --strip-components=1

    local tiff_args=()
    if [[ "$WITH_GDAL" -eq 1 ]]; then
        tiff_args=(
            "-DTIFF_INCLUDE_DIR=$PREFIX/include"
            "-DTIFF_LIBRARY=$PREFIX/lib/libtiff.a"
        )
    else
        tiff_args=("-DENABLE_TIFF=OFF")
    fi

    cmake_build "$src" \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=OFF \
        -DBUILD_APPS=OFF \
        -DBUILD_PROJSYNC=OFF \
        -DEMBED_RESOURCE_FILES=ON \
        -DUSE_ONLY_EMBEDDED_RESOURCE_FILES=ON \
        -DSQLITE3_INCLUDE_DIR="$PREFIX/include" \
        -DSQLITE3_LIBRARY="$PREFIX/lib/libsqlite3.a" \
        "${tiff_args[@]}" \
        -DENABLE_CURL=OFF

    info "PROJ installed to $PREFIX"
}

# ---------------------------------------------------------------------------
# 4. GDAL
# ---------------------------------------------------------------------------

build_gdal() {
    header "Building GDAL $GDAL_VERSION"

    local tarball="gdal-${GDAL_VERSION}.tar.gz"
    local url="https://github.com/OSGeo/gdal/releases/download/v${GDAL_VERSION}/${tarball}"

    download "$url" "$DOWNLOAD_DIR/$tarball" "$GDAL_SHA256"

    local src="$BUILD_DIR/gdal"
    clean_build_dir "$src"
    mkdir -p "$src"
    tar -xzf "$DOWNLOAD_DIR/$tarball" -C "$src" --strip-components=1

    cmake_build "$src" \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_TESTING=OFF \
        -DBUILD_APPS=OFF \
        -DEMBED_RESOURCE_FILES=ON \
        -DGDAL_BUILD_OPTIONAL_DRIVERS=OFF \
        -DGDAL_ENABLE_DRIVER_GTIFF=ON \
        -DGDAL_ENABLE_DRIVER_WMS=ON \
        -DOGR_BUILD_OPTIONAL_DRIVERS=OFF \
        -DOGR_ENABLE_DRIVER_GPX=ON \
        -DOGR_ENABLE_DRIVER_DXF=ON \
        -DOGR_ENABLE_DRIVER_OSM=ON \
        -DOGR_ENABLE_DRIVER_CSV=ON \
        -DGDAL_USE_JPEG=OFF \
        -DGDAL_USE_PNG=OFF \
        -DGDAL_USE_CURL=OFF \
        -DGDAL_USE_GEOS=OFF \
        -DGDAL_USE_GEOTIFF=ON \
        -DGDAL_USE_TIFF=ON \
        -DGDAL_USE_ICONV=OFF \
        -DGDAL_USE_ZLIB=ON \
        -DGDAL_USE_ZSTD=OFF \
        -DGDAL_USE_LZMA=OFF \
        -DGDAL_USE_WEBP=OFF \
        -DGDAL_USE_OPENJPEG=OFF \
        -DGDAL_USE_HEIF=OFF \
        -DGDAL_USE_ARROW=OFF \
        -DGDAL_USE_PARQUET=OFF \
        -DGDAL_USE_HDF5=OFF \
        -DGDAL_USE_NETCDF=OFF \
        -DGDAL_USE_LIBXML2=OFF \
        -DGDAL_USE_POSTGRESQL=OFF \
        -DGDAL_USE_MYSQL=OFF \
        -DGDAL_USE_SPATIALITE=OFF \
        -DGDAL_USE_PCRE2=OFF \
        -DGDAL_USE_OPENSSL=OFF \
        -DGDAL_USE_CRYPTOPP=OFF \
        -DGDAL_USE_XERCESC=OFF \
        -DGDAL_USE_SFCGAL=OFF \
        -DGDAL_USE_LERC=OFF \
        -DGDAL_USE_BLOSC=OFF \
        -DGDAL_USE_LZ4=OFF \
        -DGDAL_USE_BRUNSLI=OFF \
        -DGDAL_USE_BASISU=OFF \
        -DGDAL_USE_LIBKML=OFF \
        -DGDAL_USE_POPPLER=OFF \
        -DGDAL_USE_PDFIUM=OFF \
        -DTIFF_INCLUDE_DIR="$PREFIX/include" \
        -DTIFF_LIBRARY="$PREFIX/lib/libtiff.a"

    info "GDAL installed to $PREFIX"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

build_sqlite
if [[ "$WITH_GDAL" -eq 1 ]]; then
    build_tiff
fi
build_proj
if [[ "$WITH_GDAL" -eq 1 ]]; then
    build_gdal
fi

header "All dependencies built successfully"
info "Install prefix: $PREFIX"
info "Contents:"
ls -la "$PREFIX/lib/"*.a 2>/dev/null || true
