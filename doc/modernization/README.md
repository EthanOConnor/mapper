# Mapper modernization architecture

Mapper's modernization foundation is complete. Historical checkpoint evidence
is retained in this directory; this file describes the live product topology.

## Runtime layers

Dependencies point in one direction:

1. Maps, objects, symbols, templates, undo, and file formats own domain state.
2. `MapRenderSnapshot` publishes immutable, revisioned drawable state and
   structurally shares unchanged objects and geometry after edits.
3. `QtRenderScene` records ordered Qt-native operations. It is a retained
   display list, not a backend-neutral interchange format.
4. `FramePlanner` composes map, template, grid, and viewport scenes into an
   immutable `FramePacket` with a monotonic frame id.
5. `QtCanvas` presents ordinary frames through Qt Canvas Painter. QPainter
   consumes the same scene for print, PDF, deterministic reference output, and
   the uncommon composition fallback.

The render callback never reads a mutating `Map`. A frame owns all paths,
images, transforms, colors, and pass metadata needed to complete its work.

## Qt-native path representation

`QtRenderPath` is immutable and contains both of Qt's useful path forms:

- `QCanvasPath` is the primary screen representation. Mapper's ordinary line,
  area, grid, track, and overlay geometry emits it directly at geometry origin
  alongside the companion QPainterPath. The immutable path is retained by the
  scene and submitted directly to Canvas Painter without intermediate geometry
  translation.
- `QPainterPath` supports geometry queries, print/PDF, software composition,
  and Qt 6.12's arbitrary stencil clip seam, whose public Canvas entry point
  accepts only Qt's internal `QVectorPath` representation.

There is no renderer-neutral path/color/transform/image model and no per-frame
conversion into Canvas paths. Qt values (`QColor`, `QPen`, `QRectF`,
`QTransform`, and `QImage`) flow end to end.

Qt APIs that only return `QPainterPath`—font glyph outlines and external
overlay paths—cross one explicit compatibility boundary and are converted once
when their immutable `QtRenderPath` is created. The renderer never uses Canvas
Painter's compatibility `addPath(QPainterPath)` overload.

## Canvas Painter presentation

`QtCanvas` is an ordinary `QCanvasPainterWidget` child of `MapWidget`. Qt owns
the native window, input routing, screen changes, QRhi lifecycle, and backend
selection. Mapper no longer owns a native child `QWindow`, platform handles,
an external presentation thread, or a cross-language renderer bridge.

The common screen path uses Metal on macOS, D3D11 on Windows, and Qt's selected
QRhi backend elsewhere. Qt 6.12's transformed path-group cache scales stroke
widths and fill antialias fringes twice, so Mapper submits retained
`QCanvasPath` objects directly and lets Canvas prepare their vertices in device
space. This retains the native path and avoids all per-frame QPainterPath
translation while preserving correct one-pixel antialiasing. Raster templates
are retained as `QCanvasImage` resources by immutable `QImage` identity and
request mipmaps for minification.

Canvas Painter 6.12 still lacks native dashed strokes and Multiply. Mapper
handles those bounded gaps as follows:

- dashed overlay strokes are expanded once per retained scene;
- clipped area line patterns are intersected once per retained scene and then
  submitted as ordinary fill geometry, avoiding a transient stencil clip on
  every frame;
- arbitrary clips use Canvas Painter's stencil clip entry point with the
  companion `QPainterPath`. Mapper explicitly clears and rebuilds nested
  stencil clips on pop because Canvas stencil state is not part of the
  painter save/restore stack;
- a frame containing Multiply/overprint composition is rendered by
  `QPainterFrameRenderer` into one `QImage`, then uploaded and presented by the
  same Qt canvas widget;
- ordinary translucent template passes use Canvas offscreen canvases and are
  composited with opacity on the GPU.

The fallback is selected from immutable frame metadata. It is not a second
screen backend or a runtime renderer preference.

## Correctness and retention rules

- Color priority and pass order are stable.
- A camera change updates only `FrameView::world_to_viewport`; immutable paths
  remain retained.
- Pass opacity is applied exactly once. Isolated content is drawn opaque into
  its intermediate target and opacity is applied during composition.
- Overprint separations remain isolated before Multiply composition so knockout
  white affects only its own separation.
- Image admission is bounded per frame. Incomplete raster plans converge by
  publishing later immutable packets when source data becomes ready.
- QPainter remains the reference for map semantics and owns the PDF miter-limit
  correction required by Qt's PDF engine.

## Build and dependency boundary

Qt 6.12 and `Qt6::CanvasPainter` are required. Canvas Painter and its Qt Quick,
QML, OpenGL, and QRhi runtime closure must be present in every package. The
Vello, wgpu, Rust, Corrosion, CXX bridge, and custom native-surface dependency
chain has been deleted.

Build output belongs under `build/<preset>`. Configure, build, and test with the
same CMake presets used by CI; see `INSTALL.md`.

## Validation

The default build contains focused tests for:

- snapshot immutability and geometry sharing;
- direct QCanvasPath presentation, transformed stroke/fill quality, and
  stencil-state restoration;
- QPainter packet parity and overprint semantics;
- real `QCanvasPainterWidget` presentation and image residency;
- template ordering, raster admission, transparency, and tile seams;
- ordinary Qt child-widget lifecycle and input transparency.

`render_benchmark` measures edit publication, scene recording, and frame
planning. `qt_canvas_benchmark` opens a real Canvas Painter widget and reports
both paint-callback CPU time and submit-to-`frameSubmitted` cadence.

The prior Vello recovery architecture and its exact checkpoint evidence remain
historical context in `foundation-closeout-plan.md` and
`../manual/rewrite-foundation-acceptance.md`; they are not live instructions.
