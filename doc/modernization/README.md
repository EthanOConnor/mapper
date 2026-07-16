# Mapper modernization campaign

The campaign began on an internal implementation line and was promoted without
rewriting history to the public product branch `main`. It starts at upstream
commit `064e6c943ee963277f1e930bda595723acd3e8c6` (2026-06-20). Upstream is the
ancestry and behavioral baseline. `full-speed-ahead` is evidence about what was
tried, what looked good, what failed, and what was measured. It is not a code
base to merge or mechanically replay.

The implementation is a deep rewrite from current upstream. A piece of
exploratory code survives only if, after re-deriving its responsibility, it is
still the simplest readable solution. Historical structure has no presumption
of survival.

## Foundation status

The modernization foundation was completed and published at
`modernization-foundation-final`. The
[`foundation closeout plan`](foundation-closeout-plan.md), its
[`agent prompt`](foundation-closeout-agent-prompt.md), and the acceptance record
are historical design and verification context. The architecture and product
scenarios below remain current guidance for descendants of the tag.

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

## Architectural recovery layers

These seven items describe the architecture recovered by one sustained
campaign. They are layers, not product phases or a count of checkpoint tags.
The feature freeze ended when `modernization-foundation-final` became public.

1. Modern CMake/dependency model and Qt 6 application stand-up.
2. Immutable render snapshot/IR and QPainter reference renderer.
3. Renderer contract and native-platform presentation boundary.
4. Vello backend with retained vector scenes and a render thread.
5. Raster/tile scheduling, residency, filtering, and ordered composition.
6. Existing interaction, overlays, printing/export, and packaging on the new
   paths.
7. Delete transitional paths and enforce the intended dependency direction by
   the structure of the build targets and focused tests.

Numbered annotated checkpoint tags record multiple buildable, tested public
recovery points across these layers. Dated commands, test discovery counts,
measurements, artifacts, and genuine remaining failures belong in the
acceptance record for the exact tagged revision. The tags create useful
recovery points without turning the campaign into incremental product delivery
or a permanent migration architecture.

## Target architecture

Dependencies point in one direction:

1. **Domain:** maps, objects, symbols, templates, undo, and formats.
2. **Render snapshot:** immutable, revisioned drawable state.
3. **Render IR:** ordered geometry, color, clip, blend, and image operations;
   production text is shaped by Qt and frozen into ordinary paths.
4. **Frame planner:** camera, visibility, retained scenes, raster order, and
   overlays; emits an immutable frame packet.
5. **Backends:** QPainter reference/output and Vello screen rendering.
6. **Presentation:** Qt window lifecycle, native handles, input routing, and
   frame submission.

The map-to-snapshot render path knows nothing about backends, GPU resources,
native surfaces, or widgets. The IR contains no `QPainter`, QRhi, wgpu, Vello,
or platform handles. Presentation does not own maps, snapshots, spatial policy,
or caches.

### Snapshot and IR

An edit publishes a new document revision. Workers read an immutable render
snapshot, never a live mutable `Map` or spatial index. Snapshots share unchanged
renderable and path data between revisions; a single edit must not clone
the whole map or hold a global map lock while workers render.

The small IR expresses transforms, clips, fills, strokes, opacity,
blend/overprint semantics, images, line patterns, and world bounds. Paths have
explicit shared ownership. Map text, overlay labels, and track labels are
shaped with Qt while their IR is built and frozen into the same immutable fill
and stroke paths as other geometry before that IR enters a `FramePacket`. The
backend therefore needs no font registry or font-byte ownership contract.

QPainter consumes this IR as the deterministic reference, headless, print, and
PDF path. Once the cutover is complete there is no second direct
model-to-QPainter map-output path. Vello screen and offscreen rendering use a
fixed Area-AA policy. QPainter begins with its caller's render hint;
`ForceAntialiasing` commands may enable antialiasing only when the caller allows
it, and an explicit `DisableAntialiasing` request dominates.

### Retained vector rendering

Camera movement is a frame transform, not a reason to rebuild geometry. When a
map IR is rebuilt, the immutable snapshot preserves color/object order and
filters object and renderable extents against the requested bounds. The
resulting map pass and encoded scene remain retained while the snapshot and
render request are unchanged. Each drawable paints once per frame: clipped
duplicate geometry is forbidden because translucent copies double-blend at page
boundaries. Ordering-sensitive content retains stable map color/z order.

A frame packet owns references to everything it uses, so background completion
or eviction cannot destroy a scene mid-frame. Snapshots structurally share
unchanged object and path data across edits; the frame planner reuses map passes
only while the snapshot and relevant render request remain unchanged. Raster
convergence may install bounded intermediate packets as ready images are
admitted; `TemplateLayerPlan` reports coverage and admission progress so the
product coordinator either continues or waits for the existing source-ready
notification.

### Vello and presentation

Rust owns the wgpu instance, adapter, surface, device, queue, Vello renderer,
GPU scene handles, GPU image/atlas residency, composition, present, and render
thread.
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
the camera, viewport/DPR, ordered IR passes, document revision, and monotonic
frame id. Pass isolation is explicit: an overprint
separation is rendered to transparent intermediate storage before multiply
composition, so knockout white can erase earlier marks within that separation.
At that recovery point QPainter consumed the packet in `MapWidget`; checkpoint
6 replaced that temporary screen path with Vello while retaining QPainter as
the exact reference and output backend.

The corresponding `NativeSurfaceWindow` uses public Qt APIs only. It owns a
render-only `QWindow`, emits sequenced unavailable/hidden/exposed/suspended
state, reports physical size and device-pixel ratio, and exposes Qt's opaque
`WId` plus the public XCB/Wayland application display handle where required. It
contains no map, snapshot, cache, renderer, input policy, render policy, or
private `qpa` dependency. Its single input callback lets the canvas translate
native window events to the parent widget without teaching the surface about
Mapper.
Presentation readiness and surface-loss recovery use bounded event-driven
retries because those are ordinary lifecycle states.

Checkpoint 4 connects this boundary to a typed `cxx` bridge and Vello 0.9.0.
Rust owns the wgpu instance, surface, adapter, device, queue, Vello renderer,
render thread, and presentation. `RenderIR` instances are encoded once and held
as shared retained scenes while their immutable C++ source remains alive. The
ordered lifecycle channel is bounded independently from the capacity-one,
latest-wins frame channel; synchronous offscreen requests use their own bounded
reliable channel. The QWidget host contains no map, planner, cache policy, or
backend selection. Its native child owns platform hit-testing so standard and
custom cursors work; the host forwards mouse, wheel, trackpad, tablet, touch,
enter/leave, and key events once to its parent input authority.

On macOS, public raw-window APIs require deriving the AppKit surface
from the `NSView` on the main thread. Only instance/surface preparation occurs
there; the prepared wgpu objects move to and remain owned by the render thread.
Windows, XCB, and Wayland descriptors likewise use public Qt native interfaces.
On Android the application traverses the public `ViewGroup` represented by the
Qt `WId`, reuses Qt's existing `SurfaceView` or `TextureView`, and converts its
public Java `Surface` with `ANativeWindow_fromSurface`. Acquisition runs on the
Android main thread through `QAndroidApplication`; no Qt-private reflection,
second rendering view, or QPA header is involved.

The renderer first draws to an RGBA8 storage texture, then uses wgpu's maintained
`TextureBlitter` for the swapchain format. The same render-description path
supports deterministic GPU readback for conformance tests; the offscreen path
owns its readback command encoder. The complete-operation fixture and five
map/DPR/rotation/overprint scenarios require
mean channel delta below 3 and fewer than 2 percent high-delta pixels against
the QPainter reference. The initial complete-map benchmark records 10,736
commands once and includes GPU synchronization plus a 3 MiB CPU readback on
every measured frame; that deliberately stricter offscreen measurement is not
substituted for later native-presentation latency measurement. In the checkpoint
4 Release build, one unmeasured warmup preceded 300 samples at 1024 x 768 on an
Apple M3 Max; they measured 6.67 ms p50, 9.08 ms p95, and 10.91 ms maximum for
that render-and-readback path while encoding the retained scene exactly once.

### Raster

Checkpoint 5 uses the GDAL 3.13 fine-grained C++ API directly:
`GDALDatasetUniquePtr`, `GDALDataset::Open`, `GDALDataset::RasterIO`, and
`GDALDataset::IsThreadSafe`. Raster datasets are opened with the thread-safe
read-only contract and decoded on a private Qt `QThreadPool` of at most four
workers. No compatibility C-handle layer or custom worker-thread framework was
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
The template plan reports both coverage completeness and image-admission
progress to the product frame coordinator. A frame that admits ready images
continues through coalesced product requests; one waiting for source pixels
sleeps until the existing source-ready notification.

On the Apple M3 Max checkpoint machine, the Release heavy-raster gate used the
831 MiB Fishtrap source at 2068 x 1906 physical pixels. One unmeasured warmup
preceded 300 synchronized Vello render-and-readback samples. Exact visible
coverage arrived in 3.24 s; the retained frame held 25 images and 27
raster-scene commands. Samples measured 5.14 ms p50, 5.27 ms p95, and 7.40 ms
maximum. Against the QPainter reference,
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
silently changing appearance. The frame planner retains a map pass while its
snapshot and render request remain unchanged, assigns monotonic ids even to
empty frames, and installs bounded intermediate raster packets while ready
images converge.

Printing, preview, PDF, raster output, map separations, grids, and templates now
consume immutable render snapshots and IR through the QPainter output backend.
The former direct `Map::draw`, template drawing, selection drawing, and
MapWidget backing-store paths have been deleted. Surface exposure, resize,
occlusion, suspension, loss, and Android view readiness are recoverable
presentation states; actual renderer failures remain fatal and visible.

Desktop tests exercise direct native-surface creation, lifecycle ordering,
overlay fidelity, retained template composition, and shared print semantics.
The Android arm64 package is cross-built from the same CMake preset and public
native bridge. Exact test discovery counts and package evidence are kept in the
dated acceptance record rather than this evergreen architecture summary.

### Final dependency cut

Checkpoint 7 deletes the remaining migration-shaped drawing surface. `MapGrid`
has only an IR builder. `MapRenderables` only builds immutable IR; it no longer
accepts an overlay or painter. Selection and editing previews are composed by
`MapWidget`, while `Map` exposes only the selected document IR. The obsolete
QPainter declarations and includes left behind by the screen cutover are gone,
as is the last transitional spin-box overload.

Document mutation no longer stores or calls `MapWidget*`. `Map` emits typed
redraw, dirty-area, selection-visibility, and template-visibility requests;
each presentation subscribes while its `MapView` is attached. This removes the
core-to-widget ownership inversion and also fixes the old partial-visibility
calculation to use viewport-local bounds. A focused `Map` test exercises every
presentation request without constructing a widget.

The renderer-neutral contract is an explicit `Mapper::RenderIR` CMake target.
It has no Qt or backend link dependency. The production GPU consumer and its
Qt native-surface adapter are separate `Mapper::VelloBackend` and
`Mapper::VelloPresentation` targets, with `FramePacket` as the replacement
seam. This is deliberately a static build boundary rather than a runtime plugin
mechanism. The full runtime depends on these targets in the ordinary forward
direction. Source and test include paths and compile definitions are
target-scoped; no directory-wide CMake build state remains.
Static GDAL and ICU dependencies are resolved through their maintained imported
targets, including the image/database primitives and ICU data archive required
by the Android cross-link. This is intentionally the whole enforcement
mechanism: target structure, normal compiler/linker failures, and focused tests,
not a custom architecture checker.

The maintained gates cover the discovered desktop suite, focused
render/frame/raster/print/presentation behavior, Rust tests, clippy with
warnings denied, formatting, and Android arm64 packaging. Exact results and
artifacts are recorded against their public commits. Physical Android
surface/document verification and Windows printer output remain
release-candidate device gates, not foundation-closeout blockers.

The checkpoint-7 Release retained-vector benchmark encodes the 10,736-command
scene once, performs one unmeasured warmup, and measures 300 synchronized
1024 x 768 render/readback samples at 6.70 ms p50, 9.10 ms p95, and 10.06 ms
maximum. The checkpoint-7 Fishtrap raster gate reaches exact coverage in
3.33 s and, after one unmeasured warmup, measures 300 synchronized 2068 x 1906
samples at 4.08 ms p50, 4.28 ms p95, and 6.22 ms maximum, with 25 retained
images, 3.05 mean RGBA-channel delta, exact alpha, and five high-delta pixels.
These agree with the checkpoint-4/5 baselines and show no dependency-cut
regression.

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
- The exploratory native-glyph registry added a second text representation
  without a production caller; one immutable path representation is the
  smaller backend-neutral contract.
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

The development baseline is CMake 4.4.0, Ninja 1.13.0, C++23, Qt 6.10.3,
PROJ 9.8.1, GDAL 3.13.1, ICU 78.3, and Rust stable. Qt 6.10.3 is the newest
stable release whose normal public installer channel currently covers the
whole supported matrix; released `aqtinstall` cannot consume Qt 6.11.1's split
Windows metadata layout, so the project does not add credentials or unreleased
download machinery merely to claim it. Ninja
1.13.0 is retained because 1.13.2 was unavailable from the selected simple
PyPI install channel; a custom download path would add maintenance without
changing product behavior.

`src/render/vello/Cargo.toml` declares the Rust dependency intent and its
committed `Cargo.lock` is the exact graph authority. That graph currently uses
Vello 0.9.0 and wgpu 29.0.4. Corrosion 0.6.1 is hash-pinned by CMake as the
ordinary Cargo integration. Distributable builds use the pinned vcpkg
manifest; fast local builds may use system packages that satisfy the same
minimums. Build properties are target-scoped; there are no global include
directories, global definitions, or string-appended compiler flags.

## Corpus

The public corpus includes the example maps, overprinting map, text fixtures,
rotated patterns, symbol edge cases, raster/world-file/GeoTIFF templates,
legacy maps, large-coordinate cases, hidden/helper symbols, and undo fixtures.

No external corpus manifest is consumed by the build or test graph. Real-map
evidence is recorded directly in the dated acceptance and manual-parity
records; private local asset paths are not a public project contract.

The repeatable interaction trace and the 2026-07-14 side-by-side evidence
against `full-speed-ahead` are recorded in
`test/manual/render-parity.md`.

The current gates for accepting this rewrite as the practical foundation are
tracked with exact dated evidence in
`test/manual/rewrite-foundation-acceptance.md`.

## Maintained foundation gates

- The complete discovered CTest suite and the Rust format, clippy, and test
  gates pass at the exact candidate revision without weakened assertions.
- The complete-operation fixture and five maintained map/DPR/rotation/overprint
  scenarios compare Vello with QPainter at a mean channel delta below 3 and
  fewer than 2 percent high-delta pixels.
- Raster tests enforce ordered composition, bounded four-image admission,
  missing-source wakeup without polling, stable retained-image identity, and
  fractional-transform gutter coverage. The heavy-raster executable retains
  its pass/fail image comparison; dated runs record exact coverage time,
  latency, memory, and image-delta results.
- CMake presets build, test, and package the desktop targets and cross-build the
  Android arm64 APK/AAB in the GitHub-hosted matrix. Package and dependency
  notice evidence, exact run links, and artifact identities are recorded in the
  acceptance report. Build provenance remains a least-privilege, version-tag
  release path and is not claimed for ordinary candidate runs.
- Checkpoint 19 records the completed live three-map verdict covering graphic
  quality, fluidity, blanking and seams, cursors, selection and drawing,
  pan/zoom/rotation, and overlays. No product validation control plane is kept
  after that gate.
- Dated renderer benchmarks name their hardware, viewport, DPR, fixture,
  warmup, build type, and samples. They are regression evidence, not an
  invented universal frame budget or soak gate.
- Physical Android surface/document behavior and Windows printer output remain
  release-candidate gates. They do not weaken the automated foundation gate or
  become silently skipped results.

Each checkpoint record names its exact commit, commands, discovered tests,
artifacts, genuine remaining failures, and the applicable benchmark or visual
evidence for that phase. A failure is not converted into a skip merely to close
a checkpoint.
