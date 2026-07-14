# Mapper modern-core campaign

This branch starts at upstream commit
`064e6c943ee963277f1e930bda595723acd3e8c6` (2026-06-20). Upstream is the
ancestry and behavioral baseline. `full-speed-ahead` is evidence about what was
tried, what looked good, what failed, and what was measured. It is not a code
base to merge or mechanically replay.

The implementation is a deep rewrite from current upstream. A piece of
exploratory code survives only if, after re-deriving its responsibility, it is
still the simplest readable solution. Historical structure has no presumption
of survival.

## Scope

This is pure modernization. There is no feature work in this campaign. GNSS,
Purple Pen support, new imagery sources, georeferencing features, course-design
features, and other product expansion stay on their own branches.

Some capabilities necessarily appear while infrastructure is replaced: a
renderer backend, a native GPU surface, or a shared export path are examples.
They are in scope only when they preserve an existing product scenario and
reduce total complexity.

This rule needs no special governance framework. Ordinary review, tests, and a
simple question are enough: does this change replace old complexity in service
of opening, editing, viewing, printing, or exporting a real map? If not, it is
probably drift.

## Product scenarios

The architecture is judged against these existing uses:

1. Open and navigate a dense vector map without blanking, seams, or stalls.
2. Select, move, reshape, draw, commit, undo, and redo while rendering updates.
3. View large raster templates through pan, zoom, rotation, opacity, and layer
   changes without blocking input or showing tile seams.
4. Preserve text, line patterns, clips, transparency, overprint, and map color
   order on screen and in output.
5. Use keyboard, mouse, trackpad, touch, focus, popups, and cursors correctly
   across a native child rendering surface.
6. Print, preview, export PDF, and export raster output from the same rendering
   semantics used on screen.
7. Build, package, start, suspend, resume, resize, and recover the application on
   its supported platforms.

## Recovery checkpoints

These are recovery points inside one sustained campaign, not product phases.
Feature work stays frozen until all are complete.

1. Modern CMake/dependency model and Qt 6 application stand-up.
2. Immutable render snapshot/IR and QPainter reference renderer.
3. Renderer contract and native-platform presentation boundary.
4. Vello backend with retained vector scenes and a render thread.
5. Raster/tile scheduling, residency, filtering, and ordered composition.
6. Existing interaction, overlays, printing/export, and packaging on the new
   paths.
7. Delete transitional paths and enforce the intended dependency direction by
   the structure of the build targets and focused tests.

Each checkpoint must build and have focused verification. Its final state is a
dedicated commit with an annotated `modernization-checkpoint-N` tag recording
the toolchain, verification commands, test totals, corpus ids, measurements,
and genuine remaining failures. Preparatory commits may exist between tags,
but work on checkpoint N+1 does not begin until checkpoint N is clean, buildable,
tested, committed, and tagged. This creates useful recovery points without
turning the campaign into incremental product delivery or a permanent migration
architecture.

## Target architecture

Dependencies point in one direction:

1. **Domain:** maps, objects, symbols, templates, undo, and formats.
2. **Render snapshot:** immutable, revisioned drawable state.
3. **Render IR:** ordered geometry, color, clip, blend, glyph, image, and object
   identity operations.
4. **Frame planner:** camera, visibility, retained scenes, raster order, and
   overlays; emits an immutable frame packet.
5. **Backends:** QPainter reference/output and Vello screen rendering.
6. **Presentation:** Qt window lifecycle, native handles, input routing, and
   frame requests.

The domain knows nothing about backends, GPU resources, native surfaces, or
widgets. The IR contains no `QPainter`, QRhi, wgpu, Vello, or platform handles.
Presentation does not own maps, snapshots, spatial policy, or caches.

### Snapshot and IR

An edit publishes a new document revision. Workers read an immutable render
snapshot, never a live mutable `Map` or spatial index. Snapshots share unchanged
renderable, path, and font data between revisions; a single edit must not clone
the whole map or hold a global map lock while workers render.

The small versioned IR expresses transforms, clips, fills, strokes, opacity,
blend/overprint semantics, glyph runs, images, line patterns, world bounds, and
stable object ids. Paths and font faces have explicit shared ownership. That
avoids Qt 6.11 `QPainterPath` deep copies and prevents each scene from embedding
the same font bytes.

QPainter consumes this IR as the deterministic reference, headless, print, and
PDF path. Once the cutover is complete there is no second direct
model-to-QPainter map-output path.

### Retained vector rendering

Camera movement is a frame transform, not a reason to rebuild geometry. The
spatial index selects retained scene pages affecting the viewport. Each
drawable paints once per frame: clipped duplicate geometry is forbidden because
translucent copies double-blend at page boundaries. Ordering-sensitive content
retains stable map color/z order across pages.

An edit keeps the last complete revision visible until replacement data is
ready. A frame packet owns references to everything it uses, so background
completion or eviction cannot destroy a scene mid-frame.

### Vello and presentation

Rust owns the wgpu instance, adapter, surface, device, queue, Vello renderer,
GPU scene handles, raster residency, composition, present, and render thread.
It renders directly to the wgpu surface hosted by a Qt `QWindow`. QRhi is not a
wgpu bridge; removing the cross-API texture handoff deletes a large class of
platform-specific synchronization and lifetime code.

The Qt side is deliberately small:

- a canvas host remains the input authority;
- a native-surface adapter owns `QWindow` expose/resize/screen/suspend state and
  native-handle extraction;
- a render controller owns snapshots, frame planning, and invalidation.

Lifecycle commands are ordered. Frame requests use bounded latest-wins
back-pressure so an obsolete camera frame may be replaced without reordering a
resize, resource release, suspend, or shutdown. Completed status is tagged with
a frame id; the caller never treats the previous frame's status as the current
submission.

Checkpoint 3 made the contract concrete as an immutable `FramePacket`. It owns
the camera, viewport/DPR, render request, ordered IR passes, document revision,
and monotonic frame id. Pass isolation is explicit: an overprint separation is
rendered to transparent intermediate storage before multiply composition, so
knockout white can erase earlier marks within that separation. At that recovery
point QPainter consumed the packet in `MapWidget`; checkpoint 6 replaced that
temporary screen path with Vello while retaining QPainter as the exact reference
and output backend.

The corresponding `NativeSurfaceWindow` uses public Qt APIs only. It owns a
render-only `QWindow`, emits sequenced unavailable/hidden/exposed/suspended
state, reports logical and physical size, and exposes Qt's opaque `WId` plus
the public XCB/Wayland application display handle where required. It contains
no map, snapshot, cache, renderer, input forwarding, render policy, or private
`qpa` dependency. Presentation readiness and surface-loss recovery use bounded
event-driven retries because those are ordinary lifecycle states.

Checkpoint 4 connects this boundary to a typed `cxx` bridge and Vello 0.9.0.
Rust owns the wgpu instance, surface, adapter, device, queue, Vello renderer,
render thread, and presentation. `RenderIR` instances are encoded once and held
as shared retained scenes while their immutable C++ source remains alive. The
ordered lifecycle channel is bounded independently from the capacity-one,
latest-wins frame channel; synchronous offscreen requests use their own bounded
reliable channel. The QWidget host stays transparent to input and contains no
map, planner, cache policy, or backend selection.

On Apple platforms, public raw-window APIs require deriving the AppKit surface
from the `NSView` on the main thread. Only instance/surface preparation occurs
there; the prepared wgpu objects move to and remain owned by the render thread.
Windows, XCB, and Wayland descriptors likewise use public Qt native interfaces.
On Android the application traverses the public `ViewGroup` represented by the
Qt `WId`, reuses Qt's existing `SurfaceView` or `TextureView`, and converts its
public Java `Surface` with `ANativeWindow_fromSurface`. Acquisition runs on the
Android main thread through `QAndroidApplication`; no Qt-private reflection,
second rendering view, or QPA header is involved.

The renderer first draws to an RGBA8 storage texture, then uses wgpu's maintained
`TextureBlitter` for the swapchain format. The same path supports deterministic
GPU readback for conformance tests without creating a second encoder. The
complete-operation fixture and five map/DPR/rotation/overprint scenarios require
mean channel delta below 3 and fewer than 2 percent high-delta pixels against
the QPainter reference. The initial complete-map benchmark records 10,736
commands once and includes GPU synchronization plus a 3 MiB CPU readback on
every measured frame; that deliberately stricter offscreen measurement is not
substituted for later native-presentation latency measurement. In the checkpoint
4 Release build, 300 samples at 1024 x 768 on an Apple M3 Max measured 6.67 ms
p50, 9.08 ms p95, and 10.91 ms maximum for that render-and-readback path while
encoding the retained scene exactly once.

### Raster

Checkpoint 5 uses the GDAL 3.13 fine-grained C++ API directly:
`GDALDatasetUniquePtr`, `GDALDataset::Open`, `GDALDataset::RasterIO`, and
`GDALDataset::IsThreadSafe`. Raster datasets are opened with the thread-safe
read-only contract and decoded on a private Qt `QThreadPool` of at most four
workers. No compatibility C-handle layer or application-owned worker fleet was
introduced.

Useful native blocks are retained. A large strip or monolithic source instead
gets a logical 512 x 512 grid, so the 13,746 x 15,855 Fishtrap GeoTIFF is never
allocated as one image. The screen queue is capped at 64 tasks and orders a
coarse-to-exact power-of-two ladder around the view center. Decimated reads use
GDAL's maintained average resampler. A non-overlapping pan or level change
increments a generation, clears queued work, and cancels in-flight
`RasterIO`; small overlapping pans retain useful requests. Exact decoded tiles
use a 128 MiB Qt LRU per template. Mapper also supplies a 128 MiB default for
GDAL's process-global block cache, while preserving an explicit user or
environment override. This replaces GDAL's RAM-percentage default, which was
measured at roughly 3.05 GiB peak RSS on the heavy strip fixture.

Decoded tiles include a source-pixel gutter. Fully opaque tiles overlap through
those identical gutters without antialiased clip paths. A layer containing real
source alpha is flattened once into a bounded retained mosaic, so a transparent
pixel is composited exactly once rather than double-blending in the overlap.
The fractional-transform regression compares two transparent guttered tiles
with one monolithic source; the real Fishtrap capture separately caught and
then eliminated the earlier clip-edge seam.

The raster planner preserves template order below and above the map and applies
template opacity once to an isolated retained scene. It admits at most four new
immutable images per frame. C++ image identity remains stable across scene
growth, Rust copies each image into one retained `peniko::Blob`, and stock
Vello owns atlas upload and eviction. The residency test grows from four to six
tiles and observes six retained Vello images total, not ten cumulative uploads.
`FramePacket::raster_complete` makes coarse or missing coverage explicit.

On the Apple M3 Max checkpoint machine, the Release heavy-raster gate used the
831 MiB Fishtrap source at 2068 x 1906 physical pixels for 300 synchronized
Vello render-and-readback samples. Exact visible coverage arrived in 3.24 s;
the retained frame held 25 images and 27 raster-scene commands. Samples measured
5.14 ms p50, 5.27 ms p95, and 7.40 ms maximum. Against the QPainter reference,
the two maintained bilinear samplers differed by 3.05 mean RGBA-channel levels,
with exact alpha and five high-delta pixels among 3.94 million. Peak RSS was
468 MiB and macOS peak footprint was 1.08 GiB. Checkpoint 6's
`TemplateLayerPlanner` now feeds raster, nested-map, and track templates into
the same live retained Vello frame as the vector map and transient overlays.

### Product cutover and output

Checkpoint 6 removes the screen backing images and makes the native Vello
surface the only live map-widget rendering path. A frame contains the map,
templates below and above it, the map grid, and viewport-space transient IR for
selection, tools, handles, GPS, touch, paint-on-template, print guides, and
symbol editing. These operations use the same ordered frame submission and do
not require a GPU readback or a model-to-QPainter screen escape hatch.

`OverlaySceneBuilder` translates the small retained Qt drawing vocabulary into
backend-neutral IR, including dashed strokes, standard hatch patterns, text,
and stable-DPR images. Unsupported brush kinds fail at the boundary instead of
silently changing appearance. The frame planner reuses unchanged map passes,
assigns monotonic ids even to empty frames, and preserves the last complete
frame while replacement work is pending.

Printing, preview, PDF, raster output, map separations, grids, and templates now
consume immutable render snapshots and IR through the QPainter output backend.
The former direct `Map::draw`, template drawing, selection drawing, and
MapWidget backing-store paths have been deleted. Surface exposure, resize,
occlusion, suspension, loss, and Android view readiness are recoverable
presentation states; actual renderer failures remain fatal and visible.

Desktop tests exercise direct native-surface creation, lifecycle ordering,
overlay fidelity, retained template composition, and shared print semantics.
The Android arm64 package is cross-built from the same CMake preset and public
native bridge; a physical-device surface smoke remains mandatory before the
campaign can claim Android runtime acceptance.

Checkpoint 6 closes with 35 of 35 tests passing in both Debug and Release, the
six render/frame/raster/print tests passing under AddressSanitizer and
UndefinedBehaviorSanitizer, and Rust test, clippy, formatting, and Android
target checks clean. Qt's sequential package targets produced both an unsigned
arm64 release APK and release AAB with Java compilation and Android lint in the
gate. No physical Android device was attached for the final surface smoke.

## Lessons from the exploratory renderer

These are constraints, not code to port:

- QRhi/wgpu texture import, layout, queue, fence, blit, resize, and teardown
  fixes point to one end-to-end wgpu surface owner.
- Rendering from the GUI thread harmed input latency; GPU work belongs on its
  own thread.
- Stale asynchronous status once tore down a healthy surface; completions must
  be frame-indexed and genuinely complete.
- Camera-relative rasterized tiles produced jumps and seams; retained geometry
  stays in world space.
- Chunk-clipped translucent objects double-blended; each drawable paints once.
- Native Vello glyph runs and a global content-addressed font registry replace
  per-scene glyph outlines and repeated font binaries.
- Vello image minification needs source overviews or a CPU pyramid; GDAL
  decimation needs an explicit low-pass resampler.
- Per-tile opacity darkened gutter overlaps; template opacity is applied once.
- Native child windows can swallow focus, keys, gestures, and cursors; input
  routing is a tested presentation contract.
- Hidden/exposed, suspend/resume, and surface loss are normal states, not ad hoc
  error branches.
- A nonblank screenshot and toy fixtures do not prove render correctness.

## Ponytail dependency rule

Before writing a general facility, use a mature maintained package if it makes
the result smaller and clearer. Initial choices are Qt 6 Widgets, modern CMake
and Ninja, PROJ, GDAL, Clipper2, Vello/wgpu and their kurbo/peniko stack,
Corrosion for Cargo integration, and generated C++/Rust bindings (`cxx` is the
first choice). Bounded channels should use a maintained channel package rather
than home-grown synchronization.

Do not add a package for a tiny readable standard-library helper or pull in a
second application framework. Prefer imported/package targets over copied
source. A local patch needs a removal condition and a test.

The development baseline is CMake 4.4.0, Ninja 1.13.0, C++23, Qt 6.11.1,
PROJ 9.8.1, GDAL 3.13.1, ICU 78.3, and Rust stable with committed `Cargo.lock`.
Distributable builds use the pinned vcpkg manifest; fast local builds may use
system packages that satisfy the same minimums. Build properties are
target-scoped; there are no global include directories, global definitions, or
string-appended compiler flags.

## Corpus

The public corpus includes the example maps, overprinting map, text fixtures,
rotated patterns, symbol edge cases, raster/world-file/GeoTIFF templates,
legacy maps, large-coordinate cases, hidden/helper symbols, and undo fixtures.

`test/acceptance/corpus.example.toml` defines a tiny manifest for local real
assets. `corpus.local.toml` is ignored by Git. The current machine's manifest
contains Fishtrap Lake OMAP/OCD/PDF and its 831 MB raster, dense Kelsey
Creek/Wilburton contours, a legacy Kelsey OCD map, and a Wilburton raster/object
fixture. Final acceptance expands this to at least 50 real maps across ISOM,
ISSprOM, ISSkiOM, MTBO, urban/sprint, dense contours, imported OCD, and raster
templates. Reports publish ids, tags, hashes, and aggregate results, never
private files.

## Acceptance thresholds

### Correctness

- Existing upstream tests pass without weakened assertions.
- OMAP/XMAP/OCD open, save/reload, and undo/redo preserve behavior.
- Every IR operation has QPainter and Vello conformance coverage.
- Fixed reference goldens are stable. Cross-backend comparisons use edge-aware
  metrics: SSIM at least 0.995, at least 99.5% of non-edge pixels with
  CIEDE2000 at most 2, and no unexplained error region wider than two physical
  pixels.
- Clips, fill rules, transparency, overprint, patterns, glyphs, raster gutters,
  and page seams pass at DPR 1 and 2, rotations, and fractional zooms.
- Pan/zoom/style/edit transitions never show a blank frame, wrong revision,
  destroyed resource, or translucent seam.
- Print/PDF preserves page geometry, color intent, fonts, and raster placement.

### Performance

Measurements name hardware, viewport, DPR, corpus id, warmup, and build type.
On the reference Apple-silicon Mac at about 2068 x 1906 physical pixels:

- warm dense-vector and heavy-raster render-thread CPU p95 is at most 8.33 ms
  and p99 at most 12 ms;
- Qt frame preparation p95 is at most 1.5 ms;
- no input/lifecycle handler blocks for 8 ms or more;
- no post-warm stall exceeds 50 ms in a 10-minute stress run;
- style activation posts in under 10 ms and shows the right revision within
  100 ms on the heavy fixture;
- a single-object edit publishes its snapshot in under 5 ms p95 on a dense map
  with work proportional to affected immutable blocks.

Every supported platform must stay under a 16.67 ms p95 CPU frame budget for
the 60 Hz reference workload with zero blank frames. A four-hour scripted soak
must have bounded caches and less than 10% resident-memory growth after the
first 30 minutes; desktop high-tier process memory stays below 1.5 GB.

Open, save, print, and export may not regress more than 10% from the recorded
upstream baseline without a reviewed correctness/quality reason.

### Platform and delivery

- Clean preset builds on macOS, Windows, and Linux.
- Android arm64 cross-build and real-device surface smoke.
- Qt 6 only: no Qt 5, Core5Compat, qmake, or old superbuild/Azure path.
- Dependency/license inventory is available to packaging.
- Existing navigation, editing, overlays, print/export, and lifecycle scenarios
  pass on the new paths.
- Old backing store, old screen-QPainter path, duplicate raster paths,
  QRhi/wgpu interop, temporary backend flags, and migration adapters are deleted.

For each checkpoint, the commit, commands, test totals, corpus ids, benchmark
samples, visual artifacts, and genuine remaining failures are recorded. A
failure is not converted into a skip merely to close a checkpoint.
