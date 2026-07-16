# Qt 6.12 opportunity review

This branch prepares Mapper for a local Qt 6.12 renderer experiment without
making an unreleased Qt version the product or CI baseline. It reviews the
Mapper-relevant changes in Qt 6.8 through 6.12, adopts the small improvements
that stand on their own, and deliberately stops before implementing a Canvas
Painter backend.

## Decision

Qt 6.12 is a practical local development base for Mapper. The existing
application and Vello renderer need no architectural replumbing to build
against it. Keep the project minimum and hosted matrix on released Qt 6.10.3
for now; select the 6.12 kit explicitly for this worktree. A later child branch
can then require `Qt6::CanvasPainter` without mixing SDK migration and renderer
experimentation.

The source is compiled with `QT_ENABLE_STRICT_MODE_UP_TO=0x060b00`. That is the
highest clean boundary with the current 6.12 kit: setting it to 6.12 also
disables `QPair`, while the public Qt Sensors 6.12 headers still use `QPair`.
Keeping one honest 6.11 boundary is simpler than adding an exception around a
Qt header inconsistency.

## Changes adopted during the review

| Area | Change | Why it belongs here |
| --- | --- | --- |
| Source compatibility | Enable Qt strict mode through 6.11 and make C-string boundaries, URLs, signal contexts, and slot signatures explicit. | Removes deprecated implicit behavior and makes later Qt updates cheaper without compatibility wrappers. |
| Tests | Enable `QTEST_THROW_ON_FAIL` and `QTEST_THROW_ON_SKIP`. | A failure in a test helper now stops the test instead of accidentally continuing through invalid state. |
| XML exports | Stop `QXmlStreamWriter` after an error, propagate its message, and fail the export. | Qt 6.10 made writer errors observable; Mapper's map, IOF, and KML exporters previously reported success even after an encoding, invalid-character, or device error. |
| Locale selection | Match Qt's ordered `uiLanguages()` fallbacks against installed translations. | Preserves region and script choices such as `pt_BR`, `zh_CN`, and `zh_Hant` instead of truncating the locale to two letters. Explicit user settings remain authoritative. |
| OCD output | Explicitly null-terminate copied parameter strings. | Makes a real file-format boundary safe even when the input is a non-owning `QByteArray::fromRawData()` view. |
| Windows metadata | Set `QT_WINDOWS_APP_PROJECT_IDENTIFIER` to `org.openorienteering.mapper`. | Lets Qt 6.11+ generate the ordinary compatibility, long-path, version, and UAC manifest from the existing `qt_add_executable()` target. |
| Android metadata | Set `QT_ANDROID_COMPILE_SDK_VERSION` to 36. | Aligns the target with the SDK already installed and checked by the hosted workflow. |

The Qt-6.10-compatible entries were promoted to public `main` in
`74f19e3ba10191bf8c8037e887073ab92d49f3f3`. The Qt-6.11+-only Windows
manifest property remains on this branch until it can be inspected in a hosted
Windows package using that Qt generation. These are ordinary source and CMake
changes; there is no Qt-version adapter, migration framework, or new evidence
system.

## Relevant release-note findings

### Qt 6.8

- Mapper already has the Android 9/API 28 minimum required by Qt 6.8.
- The new CMYK image format and broader ICC transformation support are relevant
  to raster templates and print correctness. Mapper already uses Qt's CMYK PDF
  output, but raster-template normalization should be a separate, sample-driven
  color-management change rather than part of the SDK bump.
- Typed JNI APIs could remove stringly typed Android signatures. Defer that
  until after the Canvas Painter experiment because it may delete much of the
  custom native-surface JNI seam.
- `QPainterStateGuard` is useful for small leaf scopes with early returns. A
  mechanical conversion of the RenderIR interpreter would hide its deliberate
  push/pop semantics and is not an improvement.
- Qt Quick `VectorImage` and RHI improvements do not affect the current Widgets
  and Vello path, but are available to later Qt-renderer experiments.

### Qt 6.9

- QPainter pen and brush assignment optimizations are inherited without source
  changes.
- Ordered `QLocale::uiLanguages()` fallback is adopted here.
- `QThreadPool::setServiceLevel()` may help interactive GDAL tile loading on
  macOS under contention. It should be tried only with a real raster workload;
  no scheduling policy is added speculatively.
- Qt Quick render-target reuse, scenegraph buffer pooling, Metal resize fixes,
  and mobile multisampled-render-to-texture improvements strengthen the
  platform under Canvas Painter. They do not justify moving Mapper's UI to QML.
- New text and emoji shaping behavior is inherited. A text-heavy map is the
  right smoke test; changing global font-merging or typographic-metric policy
  without a concrete problem would alter map layout.

### Qt 6.10

- `QXmlStreamWriter` error reporting and stop-on-error behavior are adopted.
- Mapper already uses `QPainterPath` caching where repeated path-percentage
  queries benefit. Enabling it broadly would not cache ordinary painting,
  containment, or backend geometry.
- Increased-contrast support, macOS image color-space propagation, print
  improvements, and QPainter optimizations are inherited. Mapper's custom
  palette reapplication should change only if a live accessibility test shows
  that it blocks Qt's updates.
- Deployment plugin filtering could reduce package size, but only after
  inventorying the actual 6.12 bundle. Mapper legitimately uses several image,
  icon, print, style, platform, and likely TLS plugins.
- `QRangeModel` does not simplify Mapper's domain-specific template model.

### Qt 6.11

- The application builds cleanly without an architectural change. The earlier
  move back to 6.10 was an installer-matrix issue, not a Mapper or Qt 6.11
  source incompatibility.
- Qt Test now inhibits display sleep and App Nap during macOS tests. Use that
  standard behavior; do not add custom keep-awake machinery. It does not prove
  that an independently configured screen saver can never interfere.
- Automatic Windows manifest generation is adopted through the application
  identifier and should be checked in the first hosted Windows 6.11+ package.
- Native macOS file-dialog paths are normalized automatically. Wider CGImage,
  accessibility, Wayland, refresh, and print fixes are inherited without local
  abstractions.
- Android's newer Gradle, JDK, NDK, and 16 KiB page-size expectations already
  align with the hosted workflow. Explicit JNI exception handling is best added
  only at critical result-bearing calls and with a device test.

### Qt 6.12

- Canvas Painter moves from Technology Preview to supported and adds the
  even-odd fill rule Mapper needs for general paths and holes.
- `rcc` deduplicates identical resources automatically.
- Windows screens expose their color space from the display ICC profile. This
  is a useful future input, not automatic end-to-end display color management.
- Android Back-key default behavior changes and should be rechecked when the
  whole supported matrix moves to 6.12.
- `QStyleHints::toolTipWakeUpDelay` could replace Mapper's hard-coded symbol
  tooltip delay, but it is unrelated to the renderer and can wait.
- New QRhi drawing capabilities matter only if the Canvas experiment grows
  into a custom QRhi renderer. That growth would count against the intended
  simplicity advantage.

The reviewed notes also contain extensive changes to Graphs, GRPC, Protobuf,
HTTP Server, NetworkAuth, Multimedia, SQL, WebEngine, Quick 3D/XR, Virtual
Keyboard, embedded targets, and Apple platforms Mapper does not support. They
have no current Mapper component or product scenario and require no adaptation.

## Canvas Painter assessment

Canvas Painter is a strong experiment for Mapper, but not a drop-in production
replacement for Vello.

The fit is unusually good:

- Mapper already emits immutable, backend-neutral `RenderIR`.
- A backend can retain one `QCanvasPath` for each immutable Mapper path and
  reuse Canvas Painter's cached GPU geometry across camera transforms.
- Mapper already shapes text with Qt and freezes it into ordinary paths, so
  Canvas Painter's text limitations are not a blocker.
- Immutable raster image data maps naturally to a backend-local retained image
  cache, with optional mipmaps for downscaled templates.
- `QCanvasPainterWidget` could replace the native `QWindow`, window container,
  input forwarding, surface retries, and part of the cross-language lifecycle
  seam with an ordinary Widgets host using Metal on macOS.

The simple Widgets host has a real tradeoff: `QRhiWidget` renders through a
backing texture which Qt then composites into the window, while the Vello
surface presents directly. It may also move per-frame IR command walking back
onto the GUI thread even when path geometry is retained. Measure those costs
before considering a private-API direct swapchain.

Three gaps must be answered before it can be considered production-capable:

1. Canvas Painter's supported, publicly usable API exposes rectangular
   clipping, while Mapper uses arbitrary path clips for patterned areas.
2. Canvas Painter does not expose Multiply composition, while Mapper requires
   isolated Multiply passes for correct overprinting.
3. Canvas Painter has no direct dashed-stroke API; Mapper's transient overlays
   carry dash arrays and offsets.

Canvas Painter stores path coordinates as floats while Mapper's IR uses
doubles, so large-offset maps need a focused precision check. Stroke miter
conventions, antialiasing, and direct `QColor` handling also need comparison
against the current QPainter reference. The module depends on Qt Base and Qt
Declarative/Quick, while custom-brush build tooling uses ShaderTools. Its
Commercial-or-GPLv3 license is compatible with Mapper, but the Qt
Quick/QML/OpenGL runtime closure may enlarge the deployed application.

The first child branch should therefore:

1. add `Qt6::CanvasPainter` as a sibling screen backend consuming the existing
   `RenderIR` and frame packet;
2. start with `QCanvasPainterWidget`, not private Qt APIs or a custom QRhi
   swapchain;
3. retain paths and images by immutable source identity;
4. pre-segment the small number of dashed overlay paths if necessary;
5. prototype arbitrary clipping and the isolated Multiply seam before
   expanding feature coverage; and
6. compare correctness and interaction on the complete-operation fixture,
   Complete Map, and Fishtrap, including curb miters, selection handles,
   patterns, overprint, raster sharpness, colors, large-coordinate precision,
   GUI responsiveness, and deployed size.

If clipping and Multiply require a substantial custom QRhi renderer, Canvas
Painter is no longer buying the simplicity being tested. The branch should
make that visible early rather than hiding it behind a new abstraction layer.

## Local macOS build

The official Qt repository kit is installed at:

```text
/Users/ethan/Qt/6.12.0/macos
```

`qtpaths` reports Qt 6.12.0. The installed optional modules are ImageFormats,
Positioning, Sensors, SerialPort, and CanvasPainter. The kit occupies
approximately 1.7 GiB. This is a pre-release repository payload; it is suitable
for local evaluation, not yet a reason to move the hosted release matrix.

The build uses Mapper's required CMake 4.4 rather than Homebrew's older CMake:

```sh
env \
  CMAKE_PREFIX_PATH=/Users/ethan/Qt/6.12.0/macos:/opt/homebrew/opt/icu4c@78 \
  ICU_ROOT=/opt/homebrew/opt/icu4c@78 \
  /Users/ethan/.local/bin/cmake --fresh --preset release-macos

env \
  CMAKE_PREFIX_PATH=/Users/ethan/Qt/6.12.0/macos:/opt/homebrew/opt/icu4c@78 \
  ICU_ROOT=/opt/homebrew/opt/icu4c@78 \
  /Users/ethan/.local/bin/cmake --build --preset release-macos --parallel 4

/Users/ethan/.local/bin/ctest --preset release-macos
```

The application is produced at:

```text
build/release-macos/src/Mapper.app
```

On 2026-07-16 the release build completed, `otool` reported Qt 6.12.0 for the
linked Qt frameworks, and the full macOS CTest preset passed 36 of 36 tests in
32.55 seconds. The result is recorded here and in the handoff rather than
maintained as a separate evidence system.

## Sources

- [What's new in Qt 6.8](https://doc-snapshots.qt.io/qt6-6.12/whatsnew68.html)
- [What's new in Qt 6.9](https://doc-snapshots.qt.io/qt6-6.12/whatsnew69.html)
- [What's new in Qt 6.10](https://doc-snapshots.qt.io/qt6-6.12/whatsnew610.html)
- [What's new in Qt 6.11](https://doc-snapshots.qt.io/qt6-6.12/whatsnew611.html)
- [What's new in Qt 6.12](https://doc-snapshots.qt.io/qt6-6.12/whatsnew612.html)
- [Qt Canvas Painter overview](https://doc-snapshots.qt.io/qt6-6.12/qtcanvaspainter-index.html)
- [QCanvasPainter](https://doc-snapshots.qt.io/qt6-6.12/qcanvaspainter.html)
- [QCanvasPainterWidget](https://doc-snapshots.qt.io/qt6-6.12/qcanvaspainterwidget.html)
