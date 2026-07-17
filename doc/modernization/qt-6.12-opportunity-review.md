# Qt 6.12 renderer decision

## Outcome

Qt 6.12 is now Mapper's required baseline and Qt Canvas Painter is the sole
production screen renderer. The experiment justified a full cutover rather
than a sibling backend:

- the backend-neutral render IR was replaced by a Qt-native retained scene;
- `QCanvasPainterWidget` replaced the custom native surface and presentation
  lifecycle;
- Vello, wgpu, Rust, Corrosion, and the CXX bridge were deleted;
- Qt-native paths and images are retained directly, with paths submitted in
  device-prepared mode to avoid Qt 6.12's transformed-cache defects;
- QPainter remains only for print/PDF, deterministic reference output, and the
  bounded Multiply fallback.

The source remains compiled with `QT_ENABLE_STRICT_MODE_UP_TO=0x060b00` because
Qt Sensors 6.12 public headers still use APIs disabled by the 6.12 strict-mode
boundary.

## Why the scene boundary remains

The immutable snapshot and frame boundary is still necessary: Canvas Painter
owns the widget render callback, while Mapper's document can change between
events. The replacement is intentionally a Qt retained display list, not a
portable renderer abstraction. Commands carry `QColor`, `QPen`, `QRectF`,
`QTransform`, immutable `QImage`, and `QtRenderPath` directly.

`QtRenderPath` carries a final `QCanvasPath` for screen drawing and a companion
`QPainterPath`. Ordinary Mapper geometry builds both forms in lockstep at the
geometry source, so there is no intermediate path translation. Qt APIs which
only originate `QPainterPath`, such as glyph outlines, use a one-time retained
compatibility conversion. A QCanvasPath-only model is not viable in Qt 6.12
because it does not provide Mapper's geometry queries, and arbitrary stencil
clipping accepts `QVectorPath`, not `QCanvasPath`. Replacing those operations
with offscreen masks would be less direct and slower.

## Canvas-specific optimizations

- Ordinary geometry is emitted directly into QCanvasPath before publication;
  only QPainterPath-only Qt API output uses a one-time retained conversion.
- Stable immutable QCanvasPath objects are submitted directly. Qt 6.12 path
  groups are deliberately not used because their shader-only transform also
  scales cached stroke widths and fill antialias fringes, producing incorrect
  geometry. Direct submission prepares vertices in device space without any
  QPainterPath-to-QCanvasPath translation.
- Clipped line patterns are expanded and intersected once per immutable scene,
  then submitted as retained fill geometry. This avoids per-frame stencil
  setup and prevents Canvas's global stencil state from leaking into later
  map commands.
- Images are uploaded once by immutable identity with mipmap generation.
- Ordinary template opacity uses retained Canvas offscreen targets.
- Canvas and QPainter both receive the original Qt colors and transforms; no
  byte repacking or bridge conversion remains.

## Qt 6.12 gaps and decisions

1. **Arbitrary clips:** use Canvas Painter's stencil clip seam and the retained
   companion QPainterPath. This requires `Qt6::GuiPrivate` and therefore exact
   Qt patch-version matching, which the package already enforces. Canvas does
   not include stencil state in `save()`/`restore()`, so clip pop explicitly
   clears the stencil and reconstructs any enclosing clips with their recorded
   transforms.
2. **Dashed strokes:** expand the small transient dashed paths into retained
   fill geometry. Map symbol dashes are already explicit render geometry.
3. **Multiply:** render the uncommon overprint frame through the shared
   QPainter frame interpreter into one image and present it through Qt Canvas.
4. **Even-odd fill:** use the Qt 6.12 Canvas Painter fill-rule overload.
5. **Contour winding:** disable Canvas winding enforcement so the native
   QPainterPath contour directions shared by QCanvasPath remain authoritative.

These seams are smaller than maintaining an external renderer and do not
reintroduce backend selection or a neutral IR.

## Other adopted Qt opportunities

The parent Qt 6.12 branch also enabled strict mode through 6.11, made XML
writer failures observable, adopted ordered locale fallback, fixed OCD string
termination, enabled Qt's Windows manifest generation, and aligned Android's
compile SDK with API 36. Those changes remain part of this branch.

## Local validation kit

The reference kit is `/Users/ethan/Qt/6.12.0/macos`. Configure it with:

```sh
env \
  CMAKE_PREFIX_PATH=/Users/ethan/Qt/6.12.0/macos:/opt/homebrew/opt/icu4c@78 \
  ICU_ROOT=/opt/homebrew/opt/icu4c@78 \
  /Users/ethan/.local/bin/cmake --fresh --preset release-macos
```

Then build and test from `build/release-macos`. The manual
`qt_canvas_benchmark` requires a visible window and real QRhi backend.
