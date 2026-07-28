# Building Mapper

Mapper uses one CMake build on Linux, macOS, Windows, Android, and iOS. Ninja
drives the desktop and Android builds; iOS uses CMake's Xcode generator and the
official Qt iOS toolchain. The project baseline is CMake 4.4.0, Ninja 1.13.0,
a C++23 compiler, and Qt 6.10.3 for desktop and Android. The validated iOS
baseline is the official Qt 6.11.1 kit. The same checked-in presets drive local
and hosted builds.

The Qt 6.10.3 desktop/Android baseline is deliberate: it is the newest stable
release with a complete unauthenticated installer matrix in the public Qt
online repository. Qt 6.11.1 is stable, but released `aqtinstall` cannot
consume its split Windows metadata layout; the project does not add
credentials or unreleased download machinery just to claim a newer version.
iOS advances independently to the official Qt 6.11.1 kit because it is the
validated current static device/simulator toolchain for this foundation. The
native document picker, coordinated `UIDocument` writes, and persisted
security-scoped bookmarks are implemented by Mapper with public UIKit and
Foundation APIs. Update each CMake requirement, its CI environment, and this
guide together when changing either baseline.

## Dependencies

The distributable-build dependency set is declared in `vcpkg.json` and pinned
by its builtin baseline. It currently resolves PROJ 9.8.1, GDAL 3.13.1, and ICU
78.3. Clipper2 2.0.1 and KDSingleApplication 1.2.1 are content-addressed CMake
dependencies. Qt is installed from the official Qt binary repository. The
portable CMake and Ninja tools used by CI are pinned in
`requirements-build.txt`; weekly dependency updates cover that file too.
Ninja 1.13.0 is deliberate: 1.13.2 was unavailable from the selected simple
PyPI install channel, and a custom download path was not justified merely to
claim a newer version.

The Rust dependency intent is declared in `src/render/vello/Cargo.toml`, and
the committed `src/render/vello/Cargo.lock` is the exact dependency-graph
authority. It currently resolves Vello 0.9.0 with wgpu 29.0.4. Corrosion 0.6.1
is hash-pinned by CMake and provides the ordinary Cargo/CMake integration.

GDAL 3.13.1 temporarily uses `cmake/vcpkg/ports/gdal`; remove that overlay as
soon as the pinned vcpkg baseline provides the same or a newer release.

## Reproducible desktop build

Install Qt 6.10.3 with the Image Formats, Positioning, Sensors, and Serial Port
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

The optimized macOS build used for local renderer and live-parity acceptance
has its own ordinary preset:

```sh
cmake --preset release-macos
cmake --build --preset release-macos
ctest --preset release-macos
```

Direct Rust checks must also keep their compiler output in that active build
tree rather than creating `src/render/vello/target`:

```sh
export CARGO_TARGET_DIR="$PWD/build/release-macos/cargo/checks"
cargo fmt --manifest-path src/render/vello/Cargo.toml -- --check
cargo clippy --manifest-path src/render/vello/Cargo.toml --locked --all-targets -- -D warnings
cargo test --manifest-path src/render/vello/Cargo.toml --locked
```

## Android

Android targets API 36, has a minimum API of 28, and currently ships arm64-v8a.
Install JDK 21, Android SDK platform/build tools 36, NDK 27.2.12479018, and the
Qt 6.10.3 `android_arm64_v8a` kit. Set `QT_ROOT_DIR`, `VCPKG_ROOT`,
`ANDROID_SDK_ROOT`, and `ANDROID_NDK_ROOT`, then run:

```sh
cmake --preset ci-android
cmake --build --preset ci-android --target apk aab
```

Qt's generated `apk` and `aab` targets own Android packaging; there is no
second platform build system.

## iOS

iOS and iPadOS 18.0 are the minimum deployment targets. The application
supports both iPhone and iPad and uses UIKit with a native Metal surface for
the Vello/wgpu renderer. It is an ordinary Qt application, not a Mac Catalyst
build.

Install Xcode with the iOS SDK, CMake 4.4 or newer, Rust, Doxygen, and the
official Qt 6.11.1 iOS kit with the Image Formats, Positioning, and Sensors
modules. Install the matching Qt 6.11.1 macOS host tools as well. Release
builds also require `cargo-about` 0.9.1 for dependency notices:

```sh
cargo install cargo-about --version 0.9.1 --locked --features cli
```

The official Qt kit contains an arm64
device slice and an x86_64 simulator slice; the simulator preset intentionally
targets that supplied x86_64 slice, including when the Mac host is Apple
silicon. On Apple silicon, install Rosetta and a universal iOS Simulator runtime
before launching that x86_64 application. Add both Rust targets:

```sh
rustup target add aarch64-apple-ios x86_64-apple-ios
```

Bootstrap the manifest-owned vcpkg baseline exactly as in the desktop section,
then expose all three toolchain roots:

```sh
export VCPKG_ROOT="$PWD/.vcpkg"
export QT_ROOT_DIR="$HOME/Qt/6.11.1/ios"
export QT_HOST_PATH="$HOME/Qt/6.11.1/macos"
```

The presets use the Qt toolchain as CMake's primary toolchain and ask Qt to
chain-load vcpkg. The checked-in iOS triplets keep PROJ, GDAL, ICU, and the
other native dependencies static and aligned with the selected SDK and Rust
target. Build the simulator application with:

```sh
cmake --preset ios-simulator
cmake --build --preset ios-simulator
```

On an Intel Mac, select the matching host-tools triplet when configuring:

```sh
cmake --preset ios-simulator -DVCPKG_HOST_TRIPLET=x64-osx-release
```

This produces
`build/ios-simulator/src/Release-iphonesimulator/Mapper.app`. Install and launch
it in a booted compatible simulator with `xcrun simctl install` and
`xcrun simctl launch`. The simulator build is unsigned and is a runtime
development surface, not a distributable package.

The generic arm64 device compile used by CI is also unsigned. To reproduce
that signing-independent gate locally, use:

```sh
cmake --preset ios-device \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO
cmake --build --preset ios-device
```

`ios-release` is the delivery configuration. It enables the offline iOS manual,
complete dependency notices, and all runtime resources, but the repository
does not contain an Apple team identifier, signing identity, provisioning
profile, or export policy. The release owner supplies those externally and
archives the generated Xcode project. Every uploaded build must also receive a
positive, monotonically increasing `CFBundleVersion`; the example uses an
explicit release-system build number:

```sh
cmake --preset ios-release \
  -DMapper_IOS_BUILD_NUMBER="$MAPPER_IOS_BUILD_NUMBER"
xcodebuild \
  -project build/ios-release/Mapper.xcodeproj \
  -scheme Mapper \
  -configuration Release \
  -destination 'generic/platform=iOS' \
  -archivePath "$PWD/build/ios-release/Mapper.xcarchive" \
  DEVELOPMENT_TEAM="$MAPPER_APPLE_TEAM_ID" \
  CODE_SIGN_STYLE=Automatic \
  archive
xcodebuild \
  -exportArchive \
  -archivePath "$PWD/build/ios-release/Mapper.xcarchive" \
  -exportPath "$PWD/build/ios-release/export" \
  -exportOptionsPlist "$MAPPER_IOS_EXPORT_OPTIONS_PLIST"
```

Do not place Apple credentials or organization-specific export settings in the
repository. A signed archive or IPA is accepted only after completing
`test/manual/ios-release-acceptance.md` on physical devices.

iOS main-document Open and Save As use Mapper's UIKit
`UIDocumentPickerViewController` bridge; Save As first uses a native action
sheet to choose the map format. Mapper is not a
`UIDocumentBrowserViewController` application: the Qt application lifecycle
remains authoritative, while Files and other providers grant security-scoped
URLs. Mapper persists those URLs with Apple's public bookmark APIs and edits
the active document through a `UIDocument` subclass, which supplies coordinated
provider I/O and file presentation for its complete editing lifetime. Do not
replace this path with copied files, raw provider paths, or a second
document-browser lifecycle.

## GitHub delivery

Pull requests and `main` pushes run the maintained automated gate for every
target. The iOS matrix keeps a minimum-runtime lane on the Intel runner (Xcode
26.3 plus an iOS 18.6 simulator) and a current-SDK arm64 lane (Xcode 26.6 at
this baseline). Both create an unsigned arm64 `.xcarchive`; the compatibility
lane also builds and launches the official x86_64 simulator slice. CI does not
manufacture a signed IPA.
Pushing a `v*` tag builds the same matrix, creates GitHub artifact attestations,
and opens a draft GitHub release containing the desktop, APK, and AAB packages.
iOS signing and export of an installable candidate stay external release-owner
steps. The release remains a draft until the club's Apple, Windows, and Android signing
identities are connected and the resulting packages have passed device smoke
tests; unsigned binaries are never published automatically.

## Optional developer targets

Development builds skip the user manual by default. Enable it with
`-DMapper_BUILD_MANUAL=ON`; Doxygen is required. API documentation is outside
the default graph and can be enabled with `-DMapper_BUILD_API_DOCS=ON`, then
built with the `Mapper-api-docs` target.
