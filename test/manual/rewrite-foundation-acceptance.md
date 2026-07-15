# Rewritten Qt 6 foundation acceptance

This record covers the six gates required before treating the clean rewrite as
the practical foundation. It does not authorize feature work. The rewrite stays
in place; `full-speed-ahead` remains a behavioral and visual reference, not a
source branch to merge or transplant.

## Automated and implementation evidence

| Gate | Evidence | Status |
|---|---|---|
| Public hosted matrix and checkpoint | GitHub Actions run [29395835936](https://github.com/EthanOConnor/mapper/actions/runs/29395835936) passed Linux, macOS, Windows, and Android at `f106ee2d8ab8e0c0d419fd1ba011d2d77100a4bc`. Annotated tag `modernization-checkpoint-12-hosted-matrix` resolves to that commit. | Pass |
| Native Android documents | `344dda35` adopted Storage Access Framework `content://` identities throughout open, import, save, autosave, recent documents, and display names; `cbee73c7` completed save/reopen round trips. `document_path_t` passes and an arm64 API 36 emulator completed create, edit, save-as, close, reopen, and relaunch through the system document UI. | Implementation pass; repeat the surface/document smoke test on a physical release-candidate device |
| Scalable action icons | `cbba3d59` replaced the action bitmap set with licensed SVG assets and a single bounded icon loader. `style_t` verifies all 97 resources at DPR 2, exact requested sizes through 256 x 256, visible output, and the 256 x 256 pathological-request cap. | Pass |
| Windows print precision | `f85b0f4e` replaced the private Qt print-engine dependency with a public-Qt full-page image composed at the configured printer resolution for native Windows GDI. `map_printer_t` verifies one raster spool, exact 600-DPI target geometry, rendered ink, no accidental vector calls, and less than 0.022 mm half-pixel placement quantization. | Modern tested equivalent pass; physical printer acceptance remains a release-candidate gate |
| iOS decision | `SUPPORT.md` declares iOS unsupported until there is a maintained preset, package, runtime-surface acceptance, and release owner. No historical cross-build is carried as a support promise. | Pass |
| Renderer comparison | `test/manual/render-parity.md` records the repeatable three-map trace against `full-speed-ahead@74c364569059f8e83f1f9e2623671ff9fe2b9fff`. A fresh replay at `f106ee2d` matched the viewport, DPR, committed zooms, and final camera states and produced complete lossless phase images for all three maps. | Automated and still-image pass; live trackpad/display verdict remains open |

## 2026-07-15 native interaction correction

Live review of the rewritten app on `kelsey-dense-contours` found that its
embedded native render window won platform input hit-testing. Toolbar zoom
worked, but map panning, selection, editing, and tool cursors did not. Making
the native window input-transparent restored interactions but prevented macOS
from presenting its custom cursor, because AppKit only updates the cursor for
the native view selected by hit-testing.

`modernization-checkpoint-16-input-parity` keeps the render window as the
native input and cursor surface, then forwards its public Qt mouse, wheel,
trackpad, tablet, touch, enter/leave, and key events through `VelloCanvas` to
the parent `MapWidget`. The surface contains no Mapper behavior. The regression
test sends a real middle-button drag to the native window and requires the map
camera to pan and the presentation cursor to change from a custom bitmap to a
closed hand and back. The complete 36-test Release suite passes with this path.

A club mapper then repeated the dense-contour interaction check in the native
macOS app and accepted panning, tool selection, object interaction, zoom, and
custom cursor behavior. This is a pass for the defect and product scenario; it
does not substitute for the remaining continuous three-map comparison below.

## 2026-07-15 raster convergence closeout

The product frame coordinator now consumes the template planner's real outcome.
If an incomplete frame admits ready raster images, `MapWidget` schedules one
coalesced continuation; if the source is incomplete and admits nothing, it
waits for the existing source-ready notification instead of polling. The dead
`FramePacket::raster_complete` and request plumbing are removed.

The product-level regression starts with ten ready images, makes one external
`MapWidget::setMapView()` request, and requires all ten images to appear after
exactly three bounded planning passes (4, 4, 2). A second case holds a source
missing, proves the frame id and collection count remain unchanged while the
event loop runs, then marks the source ready and requires exactly one notified
plan to complete it.

Local evidence from the canonical `worktrees/main` checkout:

- Release configure/build used CMake 4.4.0, AppleClang 21.0.0, Qt 6.11.1,
  Rust 1.94.0, and build-owned Corrosion output under
  `build/release-macos/cargo`.
- `cmake --build build/release-macos --parallel 8` with CMake 4.4.0 rebuilt
  the complete Release tree; focused builds also covered
  `frame_pipeline_t`, `template_layer_planner_t`, and `raster_benchmark`.
- `ctest --test-dir build/release-macos --output-on-failure --parallel 8`
  passed all 36 discovered tests, including the native AppKit/Metal surface.
- The focused convergence, missing-source, bounded-admission, and provisional
  coverage cases passed. Rust formatting, clippy with warnings denied, and the
  two Rust unit tests also passed with `CARGO_TARGET_DIR` under
  `build/release-macos/cargo/checks`.

Public implementation commit
`60424e3fd6f87d7492758060d820eaa946d806b4` passed exact-SHA GitHub Actions
[run 29452783584](https://github.com/EthanOConnor/mapper/actions/runs/29452783584):
[macOS](https://github.com/EthanOConnor/mapper/actions/runs/29452783584/job/87479032959),
[Linux](https://github.com/EthanOConnor/mapper/actions/runs/29452783584/job/87479032980),
[Android](https://github.com/EthanOConnor/mapper/actions/runs/29452783584/job/87479032940),
and [Windows](https://github.com/EthanOConnor/mapper/actions/runs/29452783584/job/87479032956)
all completed successfully. Its exact package artifacts are:

- [Mapper-macOS, ID 8358295018](https://github.com/EthanOConnor/mapper/actions/runs/29452783584/artifacts/8358295018),
  74,534,334 bytes, SHA-256
  `24cd86d5c7211ca64272d0e6256cc3a22b1bac7b86f59ea0aa19b5f715db7e2c`.
- [Mapper-Linux, ID 8358356688](https://github.com/EthanOConnor/mapper/actions/runs/29452783584/artifacts/8358356688),
  87,085,439 bytes, SHA-256
  `80f41d9ca005087a9cdb985892d4652027baf9a84050272861cae9d3f702d5a2`.
- [Mapper-Android, ID 8358368297](https://github.com/EthanOConnor/mapper/actions/runs/29452783584/artifacts/8358368297),
  255,953,487 bytes, SHA-256
  `1a99ef7738a08ba6e7aa19536432ef2f5d61002668ed57fe958a0bf4efc3d8b4`.
- [Mapper-Windows, ID 8358760724](https://github.com/EthanOConnor/mapper/actions/runs/29452783584/artifacts/8358760724),
  78,157,896 bytes, SHA-256
  `5598d6ee39b81dcac09cd3f450ca0f931035d7b80ad1db693a7bb7e968313161`.

Annotated tag `modernization-checkpoint-17-raster-convergence` resolves to
evidence revision `e09e7697db655a50b7a0f3ccdf6e0c53ea55f501`. That exact
revision passed [run 29455346504](https://github.com/EthanOConnor/mapper/actions/runs/29455346504):
[macOS](https://github.com/EthanOConnor/mapper/actions/runs/29455346504/job/87487180977),
[Linux](https://github.com/EthanOConnor/mapper/actions/runs/29455346504/job/87487180998),
[Android](https://github.com/EthanOConnor/mapper/actions/runs/29455346504/job/87487180945),
and [Windows](https://github.com/EthanOConnor/mapper/actions/runs/29455346504/job/87487180956)
all completed successfully. Its exact artifacts are
[macOS 8359326872](https://github.com/EthanOConnor/mapper/actions/runs/29455346504/artifacts/8359326872),
[Linux 8359301505](https://github.com/EthanOConnor/mapper/actions/runs/29455346504/artifacts/8359301505),
[Android 8359294437](https://github.com/EthanOConnor/mapper/actions/runs/29455346504/artifacts/8359294437),
and [Windows 8359680094](https://github.com/EthanOConnor/mapper/actions/runs/29455346504/artifacts/8359680094).

## 2026-07-15 contract cleanup and boundary closeout

The Phase 2 implementation removes the dead native frame-request callback and
`UpdateRequest` path, render-command identity, the unused overprinting helper,
backend-id echo, and stale touch include. `FramePacket` is now a snapshot-free
backend input in its own header, while native surface state is a Qt-free value
separate from the `QWindow` owner. The Vello backend consumes those narrow
headers and no longer links `Qt6::Gui`.

The test-only glyph/font bridge is deleted because every production text path
already freezes Qt-shaped text into immutable paths. The ineffective separate
map and text antialiasing preferences and unsupported UIKit/iOS residue are
removed; the supported Vello screen path has one fixed high-quality policy.
QPainter still inherits its caller's antialiasing choice unless rendering is
explicitly disabled, and focused pixel coverage proves that forced-AA commands
cannot override an explicit disable. Native surface DPR remains part of the
surface value and drives resubmission after display changes.

This cleanup deliberately retains snapshot/domain object identity, Windows
WARP runtime coverage, Linux Xvfb, bounded Qt 6 icon loading, the GDAL overlay,
QPainter reference/print/export rendering, and native-surface input forwarding.

Local evidence from the canonical `worktrees/main` checkout:

- The retained Release cache names
  `/Users/ethan/dev/oom/worktrees/main` as `CMAKE_HOME_DIRECTORY`; it uses CMake
  4.4.0, Qt 6.11.1, and Rust/Cargo 1.94.0, with Corrosion output below
  `build/release-macos/cargo`.
- The complete Release build passed. On the exact final tree, a serial CTest
  run excluding the two visible-surface suites passed 34/34 tests in 15.71
  seconds. The two excluded suites separately passed their packet, convergence,
  missing-source, offscreen-GPU, map-corpus, and surface-lifecycle-state cases.
  The current desktop WindowServer session continued to return
  `TargetUnavailable` for visible AppKit/Metal drawable acquisition after
  overlapping GUI-test runs, so the exact hosted macOS job remains the
  authoritative native-presentation gate for this checkpoint.
- Rust formatting, clippy with warnings denied, and both Rust unit tests pass
  with `CARGO_TARGET_DIR=build/release-macos/cargo/checks`.
- The generated `vello_renderer.cpp` compile command contains only canonical
  source/build paths and no Qt include directory, Qt library definition, or
  autogen include. The renderer, raster, and reference benchmark targets build.
- Dead-contract searches are clean outside the authoritative closeout plan and
  legitimate domain IDs. All 64 translation catalogs pass `xmllint`, and
  `git diff --check` passes.

Exact implementation SHA, hosted jobs and artifacts, and the checkpoint-18 tag
remain pending the public exact-SHA matrices. Later phases still own measured
CI/cache simplification and source-tree Cargo cleanup, documentation and the
live checkpoint-19 renderer verdict, parity-driver retirement and the final
tag, and bounded campaign-workspace retirement.

## Remaining people and hardware checks

These are deliberately small product checks, not missing automation machinery:

1. Before accepting the rewrite as superior in practice, have a club mapper run
   the same three maps at the exact checkpoint-19 candidate SHA and
   `full-speed-ahead@74c36456` on representative hardware. Record whether pan,
   pinch, cursor-anchored zoom, text and pattern stability, crispness, display
   pacing, and input latency are at least as good in the rewrite.
2. Before shipping a release candidate, complete
   `windows-print-acceptance.md` on Windows 11 with one physical printer, a
   measured 100 mm map distance, retained driver properties, and a scan or
   calibrated photograph. This is defense against real driver behavior; the
   modernization foundation gate is already satisfied by the tested public-Qt
   precision spool.
3. Before shipping an Android release candidate, repeat the surface and
   document-access smoke test on a physical supported device.

Do not describe the rewritten foundation as superior in practice, or begin
feature work on it, until the live renderer verdict is recorded as a pass. Do
not ship a release candidate until the applicable physical-device gates pass.
A failure is a renderer or platform defect to fix in the rewrite; it is not a
reason to transplant the exploratory port wholesale.
