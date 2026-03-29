#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 6 ]; then
	echo "usage: $0 <superbuild-source-dir> <source-dir> <build-dir> <target> <app-id> <output-apk>" >&2
	exit 64
fi

SUPERBUILD_SOURCE_DIR=$(cd "$1" && pwd)
SOURCE_DIR=$(cd "$2" && pwd)
BUILD_DIR=$(mkdir -p "$3" && cd "$3" && pwd)
TARGET=$4
APP_ID=$5
OUTPUT_APK=$6

ANDROID_INSTALL_ROOT=${ANDROID_INSTALL_ROOT:-"$BUILD_DIR/android-toolchain"}
BUILD_PARALLELISM=${BUILD_PARALLELISM:-$(nproc)}
CMAKE_GENERATOR=${CMAKE_GENERATOR:-Unix Makefiles}

mkdir -p "$(dirname "$OUTPUT_APK")"

target_install_dir="$BUILD_DIR/$TARGET/install"

# Force superbuild to provision a consistent SDK / NDK instead of inheriting
# whatever the hosted runner happens to expose.
unset ANDROID_HOME
unset ANDROID_NDK_ROOT
unset ANDROID_SDK_ROOT

# RelWithDebInfo still produces a release-style APK, but avoids the superbuild
# toolchain's hard failure on unsigned pure Release packaging.
cmake -S "$SUPERBUILD_SOURCE_DIR" -B "$BUILD_DIR" -G "$CMAKE_GENERATOR" \
	-DCMAKE_BUILD_TYPE=RelWithDebInfo \
	"-DMapper_CI_SOURCE_DIR=$SOURCE_DIR" \
	"-DMapper_CI_VERSION_DISPLAY=" \
	"-DMapper_CI_APP_ID=$APP_ID" \
	"-DMapper_CI_GDAL_DATA_DIR=$target_install_dir/usr/share/gdal" \
	"-DENABLE_$TARGET=1" \
	"-D${TARGET}_INSTALL_PREFIX=/usr" \
	"-DANDROID_SDK_INSTALL_ROOT=$ANDROID_INSTALL_ROOT" \
	"-DANDROID_NDK_INSTALL_ROOT=$ANDROID_INSTALL_ROOT" \
	-DANDROID_BUILD_LIBCXX=1

cmake --build "$BUILD_DIR" \
	--target "openorienteering-mapper-ci-$TARGET" \
	--parallel "$BUILD_PARALLELISM"

apk=$(find "$BUILD_DIR/$TARGET/openorienteering-mapper-ci" -type f -name 'OpenOrienteering-Mapper-*.apk' | sort | head -n 1)
if [ -z "$apk" ]; then
	echo "no APK produced under $BUILD_DIR/$TARGET/openorienteering-mapper-ci" >&2
	exit 1
fi

cp "$apk" "$OUTPUT_APK"
