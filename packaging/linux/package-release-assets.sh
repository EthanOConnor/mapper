#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 6 ]; then
	echo "usage: $0 <source-dir> <build-dir> <dist-dir> <asset-prefix> <flatpak-id> <package-name>" >&2
	exit 64
fi

SOURCE_DIR=$(cd "$1" && pwd)
BUILD_DIR=$(cd "$2" && pwd)
DIST_DIR=$(mkdir -p "$3" && cd "$3" && pwd)
ASSET_PREFIX=$4
FLATPAK_ID=$5
PACKAGE_NAME=$6

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
APPDIR="$DIST_DIR/AppDir"
PACKAGE_ROOT="$DIST_DIR/package-root"
FLATPAK_ROOT="$DIST_DIR/flatpak-root"
FLATPAK_BUILD_DIR="$DIST_DIR/flatpak-build"
FLATPAK_REPO="$DIST_DIR/flatpak-repo"
RELEASE_DIR="$DIST_DIR/release"
LINUXDEPLOY_BIN=${LINUXDEPLOY_BIN:-linuxdeploy}

rm -rf "$APPDIR" "$PACKAGE_ROOT" "$FLATPAK_ROOT" "$FLATPAK_BUILD_DIR" "$FLATPAK_REPO" "$RELEASE_DIR"
mkdir -p "$APPDIR/usr" "$RELEASE_DIR"

cmake --install "$BUILD_DIR" --prefix "$APPDIR/usr"

mkdir -p "$APPDIR/usr/lib/$PACKAGE_NAME"
mv "$APPDIR/usr/bin/Mapper" "$APPDIR/usr/lib/$PACKAGE_NAME/Mapper.bin"
install -m 0755 "$SCRIPT_DIR/Mapper-launcher.sh" "$APPDIR/usr/bin/Mapper"

if [ -x "$BUILD_DIR/src/gdal/mapper-gdal-info" ]; then
	"$BUILD_DIR/src/gdal/mapper-gdal-info" > "$APPDIR/usr/share/$PACKAGE_NAME/mapper-gdal-info.txt"
fi

ln -sfn usr/bin/Mapper "$APPDIR/AppRun"
ln -sfn usr/share/applications/Mapper.desktop "$APPDIR/Mapper.desktop"
ln -sfn usr/share/icons/hicolor/256x256/apps/Mapper.png "$APPDIR/.DirIcon"

linuxdeploy_args=(
	--appdir "$APPDIR"
	--desktop-file "$APPDIR/usr/share/applications/Mapper.desktop"
	--icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/Mapper.png"
	--executable "$APPDIR/usr/lib/$PACKAGE_NAME/Mapper.bin"
	--plugin qt
)

while IFS= read -r plugin; do
	linuxdeploy_args+=(--library "$plugin")
done < <(find "$APPDIR/usr/lib/$PACKAGE_NAME/plugins" -type f -name '*.so' | sort)

"$LINUXDEPLOY_BIN" "${linuxdeploy_args[@]}"

cp -a "$APPDIR/usr" "$PACKAGE_ROOT/"

mkdir -p "$FLATPAK_ROOT"
cp -a "$PACKAGE_ROOT/usr/." "$FLATPAK_ROOT/"

flatpak_desktop="$FLATPAK_ROOT/share/applications/$FLATPAK_ID.desktop"
sed "s/^Icon=.*/Icon=$FLATPAK_ID/" \
	"$FLATPAK_ROOT/share/applications/Mapper.desktop" > "$flatpak_desktop"
rm -f "$FLATPAK_ROOT/share/applications/Mapper.desktop"

while IFS= read -r icon; do
	icon_dir=$(dirname "$icon")
	cp "$icon" "$icon_dir/$FLATPAK_ID.png"
done < <(find "$FLATPAK_ROOT/share/icons/hicolor" -path '*/apps/Mapper.png' | sort)

install -Dm644 \
	"$SCRIPT_DIR/org.openorienteering.Mapper.coc.metainfo.xml" \
	"$FLATPAK_ROOT/share/metainfo/$FLATPAK_ID.metainfo.xml"

desktop-file-validate "$flatpak_desktop"
appstreamcli validate --no-net "$FLATPAK_ROOT/share/metainfo/$FLATPAK_ID.metainfo.xml"

cat > "$DIST_DIR/flatpak.yml" <<EOF
id: $FLATPAK_ID
branch: stable
runtime: org.freedesktop.Platform
runtime-version: '24.08'
sdk: org.freedesktop.Sdk
command: Mapper
finish-args:
  - --share=network
  - --socket=fallback-x11
  - --socket=wayland
  - --device=dri
  - --filesystem=home
modules:
  - name: mapper
    buildsystem: simple
    build-commands:
      - install -d /app
      - cp -a ./. /app/
    sources:
      - type: dir
        path: flatpak-root
EOF

flatpak-builder \
	--force-clean \
	--user \
	--install-deps-from=flathub \
	--repo="$FLATPAK_REPO" \
	"$FLATPAK_BUILD_DIR" \
	"$DIST_DIR/flatpak.yml"

flatpak build-bundle \
	"$FLATPAK_REPO" \
	"$RELEASE_DIR/$ASSET_PREFIX.flatpak" \
	"$FLATPAK_ID" \
	stable \
	--runtime-repo=https://flathub.org/repo/flathub.flatpakrepo

release_version=$(sed -nE 's/^project\(Mapper VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$SOURCE_DIR/CMakeLists.txt" | head -n 1)
coc_version=$(sed -nE 's/^set\(Mapper_COC_VERSION "([^"]+)".*/\1/p' "$SOURCE_DIR/CMakeLists.txt" | head -n 1)
package_iteration="coc${coc_version}.1"

fpm_common=(
	-s dir
	-C "$PACKAGE_ROOT"
	-n "$PACKAGE_NAME"
	-v "$release_version"
	--iteration "$package_iteration"
	--url "https://github.com/EthanOConnor/mapper"
	--vendor "OpenOrienteering"
	--license "GPL-3.0-or-later"
	--maintainer "Ethan O'Connor"
	--description "OpenOrienteering Mapper fork release with tiled raster and online tile source support"
	usr
)

fpm \
	"${fpm_common[@]}" \
	-t deb \
	-a amd64 \
	-p "$RELEASE_DIR/$ASSET_PREFIX.deb"

fpm \
	"${fpm_common[@]}" \
	-t rpm \
	-a x86_64 \
	--rpm-os linux \
	-p "$RELEASE_DIR/$ASSET_PREFIX.rpm"

(
	cd "$RELEASE_DIR"
	rm -f ./*.AppImage
	ARCH=x86_64 "$LINUXDEPLOY_BIN" --appdir "$APPDIR" --output appimage
	appimage=$(find . -maxdepth 1 -type f -name '*.AppImage' | head -n 1)
	mv "$appimage" "$ASSET_PREFIX.AppImage"
)

test -f "$RELEASE_DIR/$ASSET_PREFIX.AppImage"
test -f "$RELEASE_DIR/$ASSET_PREFIX.deb"
test -f "$RELEASE_DIR/$ASSET_PREFIX.rpm"
test -f "$RELEASE_DIR/$ASSET_PREFIX.flatpak"
