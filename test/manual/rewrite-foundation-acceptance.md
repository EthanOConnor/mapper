# Rewritten Qt 6 foundation acceptance

This record covers the implementation, delivery, and human gates required
before treating the clean rewrite as the practical foundation. It does not
authorize feature work. The rewrite stays in place; `full-speed-ahead` remains
a behavioral and visual reference, not a source branch to merge or transplant.

## Automated and implementation evidence

| Gate | Evidence | Status |
|---|---|---|
| Public hosted matrix and checkpoint | GitHub Actions run [29468215701](https://github.com/EthanOConnor/mapper/actions/runs/29468215701) passed Linux, macOS, Windows, and Android at `fa7658a86cce2aa6a6eb2d52452c98b6f7d1da3c`, including native startup checks of all three desktop packages and structural inspection of the Android APK and app bundle. The latest public annotated checkpoint is `modernization-checkpoint-18-contract-cleanup`; its own exact tagged matrix is recorded below. | Pass |
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
- The complete Release build passed. On the exact committed tree,
  `ctest --test-dir build/release-macos --output-on-failure -j 1` passed all 36
  discovered tests in 29.98 seconds, including both visible AppKit/Metal
  presentation suites. This final serial run followed a transient occluded
  WindowServer session; no source, retry, assertion, or test change was made
  between the occluded attempt and the full pass.
- Rust formatting, clippy with warnings denied, and both Rust unit tests pass
  with `CARGO_TARGET_DIR=build/release-macos/cargo/checks`.
- The generated `vello_renderer.cpp` compile command contains only canonical
  source/build paths and no Qt include directory, Qt library definition, or
  autogen include. The renderer, raster, and reference benchmark targets build.
- Dead-contract searches are clean outside the authoritative closeout plan and
  legitimate domain IDs. All 64 translation catalogs pass `xmllint`, and
  `git diff --check` passes.

Public implementation commit
`36d44b62984ab6673075c5bce8f691ed62307fcf` passed exact-SHA GitHub Actions
[run 29460058561](https://github.com/EthanOConnor/mapper/actions/runs/29460058561):
[Android](https://github.com/EthanOConnor/mapper/actions/runs/29460058561/job/87501457660),
[macOS](https://github.com/EthanOConnor/mapper/actions/runs/29460058561/job/87501457667),
[Windows](https://github.com/EthanOConnor/mapper/actions/runs/29460058561/job/87501457679),
and [Linux](https://github.com/EthanOConnor/mapper/actions/runs/29460058561/job/87501457709)
all completed successfully. The hosted macOS CTest run passed 36/36 in 16.25
seconds, including `vello_renderer_t` and `frame_pipeline_t`. Its exact package
artifacts are:

- [Mapper-Android, ID 8360947606](https://github.com/EthanOConnor/mapper/actions/runs/29460058561/artifacts/8360947606),
  255,302,653 bytes, SHA-256
  `493a3c781a0e34eb697ca6de7dcdefa9000c2333089bf85ccf843b281f22be2b`.
- [Mapper-macOS, ID 8360952622](https://github.com/EthanOConnor/mapper/actions/runs/29460058561/artifacts/8360952622),
  74,395,574 bytes, SHA-256
  `fa53db7ef359e61979b4982fb666963864959909aef8c954b5ff6cca136d1087`.
- [Mapper-Windows, ID 8361226328](https://github.com/EthanOConnor/mapper/actions/runs/29460058561/artifacts/8361226328),
  77,957,438 bytes, SHA-256
  `b0b263a4c1708aa253a7420395d7cf4d398e5c1da5035ba833d5fd840a87aabe`.
- [Mapper-Linux, ID 8360916779](https://github.com/EthanOConnor/mapper/actions/runs/29460058561/artifacts/8360916779),
  86,922,945 bytes, SHA-256
  `7c2f1b3cfaffecf85aaead28e77534fd51096f78cdbe5d064825e3fc882da3e8`.

Annotated tag `modernization-checkpoint-18-contract-cleanup` resolves to
evidence revision `eb6aac362c4903dd5dce6698cf99af27088325e4`. That exact
revision passed [run 29461379599](https://github.com/EthanOConnor/mapper/actions/runs/29461379599):
[Android](https://github.com/EthanOConnor/mapper/actions/runs/29461379599/job/87505369101)
in 9:06,
[macOS](https://github.com/EthanOConnor/mapper/actions/runs/29461379599/job/87505369108)
in 8:02,
[Linux](https://github.com/EthanOConnor/mapper/actions/runs/29461379599/job/87505369167)
in 9:06, and
[Windows](https://github.com/EthanOConnor/mapper/actions/runs/29461379599/job/87505369175)
in 27:41 all completed successfully. The hosted macOS CTest run passed 36/36
in 13.80 seconds. Its exact package artifacts are:

- [Mapper-Android, ID 8361415466](https://github.com/EthanOConnor/mapper/actions/runs/29461379599/artifacts/8361415466),
  255,302,880 bytes, SHA-256
  `39cc439c124ee423dbaf3e31e23be797bedbb2221775964fff4aae17bbf78fa0`.
- [Mapper-macOS, ID 8361397043](https://github.com/EthanOConnor/mapper/actions/runs/29461379599/artifacts/8361397043),
  74,395,625 bytes, SHA-256
  `5414f129f053e8a24700ac87eb29b98805080c735224cd4a707f0b505ac62099`.
- [Mapper-Linux, ID 8361415591](https://github.com/EthanOConnor/mapper/actions/runs/29461379599/artifacts/8361415591),
  86,922,722 bytes, SHA-256
  `bee010a00f60d7c7f59e8c11a9e153ab83b77fc626f2bc7370f7170efda9e21d`.
- [Mapper-Windows, ID 8361739510](https://github.com/EthanOConnor/mapper/actions/runs/29461379599/artifacts/8361739510),
  77,957,433 bytes, SHA-256
  `f73d4875e5bcf64a77c9c4634a166ab28dec61946040a9f3e175bea42418aa94`.

## 2026-07-15/16 CI, cache, build-path, and package closeout

The closest pre-change no-cache observation was commit
`c1a89554ba0cd38f47116fb610ba8643889713f7` in
[run 29357764218](https://github.com/EthanOConnor/mapper/actions/runs/29357764218).
Every job missed both the broad vcpkg and Rust keys, and every Qt installation
also missed. This was diagnostic timing evidence, not a green gate: macOS
[failed a test after 37:25](https://github.com/EthanOConnor/mapper/actions/runs/29357764218/job/87169640699),
Linux [failed a test after 37:00](https://github.com/EthanOConnor/mapper/actions/runs/29357764218/job/87169640770),
Android [completed successfully in 31:32](https://github.com/EthanOConnor/mapper/actions/runs/29357764218/job/87169640752),
and Windows [was cancelled by the branch concurrency policy after 1:02:05](https://github.com/EthanOConnor/mapper/actions/runs/29357764218/job/87169640717)
while still building. Only Android reached the old vcpkg/Rust post-job saves.
The incomplete desktop jobs therefore provide cold-path measurements, not
valid end-to-end cold acceptance results.

The last green pre-change warm baseline was commit
`9e607b98939552a7c05141cdf54256242e44faa5` in
[run 29435612841](https://github.com/EthanOConnor/mapper/actions/runs/29435612841).
The broad vcpkg and Rust caches were exact hits on every job. Desktop Qt and
Android's host Qt were exact hits, but Android's target Qt cache missed and was
saved during the run:

| Hosted job | Warm duration | vcpkg restored | Rust restored | Qt restore state |
|---|---:|---:|---:|---|
| [macOS](https://github.com/EthanOConnor/mapper/actions/runs/29435612841/job/87421394320) | 8:05 | 365,045,936 B | 729,103,935 B | Hit, 312,877,303 B |
| [Linux](https://github.com/EthanOConnor/mapper/actions/runs/29435612841/job/87421394361) | 10:05 | 361,130,238 B | 793,221,422 B | Hit, 284,167,823 B |
| [Android](https://github.com/EthanOConnor/mapper/actions/runs/29435612841/job/87421394368) | 10:03 | 562,923,896 B | 652,771,328 B | Host hit, 284,167,823 B; target miss, then saved 356,459,034 B |
| [Windows](https://github.com/EthanOConnor/mapper/actions/runs/29435612841/job/87421394363) | 26:37 | 921,233,376 B | 621,162,278 B | Hit, 357,561,111 B |

Before changing the cache policy, the repository held 21 cache entries using
10,401,530,902 bytes: seven Qt entries used 2,238,070,374 bytes, eight Rust
entries used 4,666,881,808 bytes, and six vcpkg entries used 3,496,578,720
bytes. Thirteen exact-hit entries made up a 6,601,825,503-byte useful working
set; the other eight entries accounted for 3,799,705,399 bytes of stale or
churned occupancy. Measured Qt saves took only 12 to 68 seconds while consuming
2.24 GB, so the workflow no longer caches Qt and those entries were deleted.

Commit `50c64fb954dcbcf41000f70ede35dfc1758ec7bf` replaced the broad cache
layout with five explicit families per hosted job: vcpkg downloads, vcpkg
binaries, Cargo sources, the Rust target tree, and the build-owned
`cargo-about` binary. Keys include the runner image and architecture, preset or
triplet, actual Rust release and commit, actual compiler identity, vcpkg
baseline, dependency manifests and locks, overlays, Rust sources, and the
Android NDK/API identity where applicable. Mutable build outputs require exact
keys; only immutable downloads and sources may use a restore prefix. Pull
requests can restore caches but cannot write them. Saves occur only after all
tests, package checks, and artifact uploads succeed on `main` or a `v*` tag.

The same exact SHA passed both attempts of
[run 29464205798](https://github.com/EthanOConnor/mapper/actions/runs/29464205798),
first with no version-3 entries and then with the resulting entries warm:

| Hosted job | Cold attempt 1 | Warm attempt 2 | Warm restore evidence | Cold cache bytes |
|---|---:|---:|---|---:|
| [macOS](https://github.com/EthanOConnor/mapper/actions/runs/29464205798/job/87513801866) | 34:40 | [11:29](https://github.com/EthanOConnor/mapper/actions/runs/29464205798/job/87521958827) | Five exact hits in 41 s | 836,649,592 |
| [Linux](https://github.com/EthanOConnor/mapper/actions/runs/29464205798/job/87513801870) | 40:13 | [9:28](https://github.com/EthanOConnor/mapper/actions/runs/29464205798/job/87521958821) | Five exact hits in 11 s | 878,164,142 |
| [Android](https://github.com/EthanOConnor/mapper/actions/runs/29464205798/job/87513801867) | 40:21 | [8:16](https://github.com/EthanOConnor/mapper/actions/runs/29464205798/job/87521958839) | Five exact hits in 19 s | 921,843,922 |
| [Windows](https://github.com/EthanOConnor/mapper/actions/runs/29464205798/job/87513801876) | 1:05:55 | [21:31](https://github.com/EthanOConnor/mapper/actions/runs/29464205798/job/87521958820) | Five exact hits in 52 s | 1,312,871,192 |

The cold jobs saved 20 entries totaling 3,949,528,848 bytes. Every warm save
was skipped because every exact key hit. Warm configure time fell from 21:11 to
0:45 on macOS, 25:29 to 0:16 on Linux, 25:57 to 0:18 on Android, and 37:37 to
1:38 on Windows. The Windows `cargo-about` step fell from a real cold build of
3:22 to a one-second build-owned binary hit. The complete Windows warm rerun
was 21:31, versus 27:41 at checkpoint 18. This removes the previous
executable-path miss without custom runners or infrastructure.

After the obsolete cache generations were deleted and the package-smoke commit
ran, the dated inventory was 20 entries and 3,951,423,582 bytes:

| Cache family | Bytes |
|---|---:|
| vcpkg downloads | 896,159,956 |
| vcpkg binaries | 1,314,217,583 |
| Cargo sources | 331,452,318 |
| Rust targets | 1,385,442,470 |
| `cargo-about` binaries | 24,151,255 |

The retained local cache was recreated from the canonical
`/Users/ethan/dev/oom/worktrees/main` path with the ordinary `release-macos`
preset. Its `CMAKE_HOME_DIRECTORY` and generated Corrosion invocation contain
only canonical source paths and build-owned output below
`build/release-macos/cargo`; neither contains the `modern-core` compatibility
path. The source-tree `src/render/vello/target` directory is absent and is no
longer ignored. A fresh Qt 6.11.1 Release configure and complete build passed,
and the complete local suite passed 36/36 in 19.48 seconds with native
AppKit/Metal access. A newly generated DMG verified successfully and its
isolated copy remained alive for the same ten-second startup check used by CI.

The reworked workflow passed `actionlint` 1.7.12 and a separate YAML parse of
both `ci.yml` and `dependabot.yml`. All 34 action uses are immutable 40-character
SHA pins with readable release comments. The workflow has top-level
`contents: read`, one ref-scoped concurrency group with cancellation, trusted
post-gate cache writes, and no write permissions outside the `v*`-only release
job. Dependabot checks GitHub Actions, vcpkg, Cargo, and pip weekly. In exact-SHA
run 29468215701, package configuration failed closed unless dependency notices
were present and reported these inventories: macOS had 9 Qt SBOMs, 21 vcpkg
notices, 21 vcpkg SBOMs, and 1 Rust notice; Linux had 10, 21, 21, and 1;
Android had 9, 21, 21, and 1; and Windows had 9, 20, 20, and 1. The main-branch
run correctly skipped the tag-only draft-release job. That retained path grants
its three write permissions only at job scope, downloads the matrix artifacts,
attests them through the full-SHA-pinned standard provenance action, and creates
a draft release. No version tag was created here, so this record does not claim
an attestation for the foundation candidate.

Commit `fa7658a86cce2aa6a6eb2d52452c98b6f7d1da3c` made package
acceptance part of the hosted matrix before cache saves. Exact-SHA
[run 29468215701](https://github.com/EthanOConnor/mapper/actions/runs/29468215701)
passed
[macOS](https://github.com/EthanOConnor/mapper/actions/runs/29468215701/job/87525795591)
in 8:33,
[Linux](https://github.com/EthanOConnor/mapper/actions/runs/29468215701/job/87525795561)
in 12:02,
[Android](https://github.com/EthanOConnor/mapper/actions/runs/29468215701/job/87525795563)
in 12:17, and
[Windows](https://github.com/EthanOConnor/mapper/actions/runs/29468215701/job/87525795567)
in 27:31. The macOS, Linux, and Windows jobs extracted their emitted package in
an isolated location, verified required runtime payloads, and kept the packaged
application alive for ten seconds; those gates took 16, 12, and 13 seconds,
respectively. The Android job spent three seconds proving that both archives
are readable, the APK is aligned, the expected manifest, bytecode, and arm64
library entries exist, and package, SDK, and ABI metadata match the supported
configuration. Its exact artifacts are:

- [Mapper-macOS, ID 8363892795](https://github.com/EthanOConnor/mapper/actions/runs/29468215701/artifacts/8363892795),
  74,400,270 bytes, SHA-256
  `260315467a355566a3265edb639fe65ece300c08adda3cd4fdfd9e0f7b215b11`.
- [Mapper-Linux, ID 8363951316](https://github.com/EthanOConnor/mapper/actions/runs/29468215701/artifacts/8363951316),
  87,515,882 bytes, SHA-256
  `8e84ad2c68aa86dac6fa4805a13f365c50c06c848b0ef5e6f9b878a3eab4b2ac`.
- [Mapper-Android, ID 8363955264](https://github.com/EthanOConnor/mapper/actions/runs/29468215701/artifacts/8363955264),
  255,348,235 bytes, SHA-256
  `047e7700046caed330c2eb46e2573f4cfa6a66eb488f4196b68ccd5f483c4a22`.
- [Mapper-Windows, ID 8364156736](https://github.com/EthanOConnor/mapper/actions/runs/29468215701/artifacts/8364156736),
  77,978,967 bytes, SHA-256
  `ed7d809d92305549ef335a798625850fae257553466b6358528dc70ab9736cd7`.

The checkpoint-19 candidate matrix and live renderer verdict remain ahead.
Only after that human pass may the product-embedded parity driver be retired
and the final tag and bounded campaign-workspace retirement proceed.

## Remaining people and hardware checks

These are deliberately small product checks, not missing automation machinery:

1. Before accepting live renderer parity, have a club mapper run the same three
   maps at the exact checkpoint-19 candidate SHA and
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

Do not treat the rewritten foundation as live-accepted until the renderer
verdict is recorded as a pass, and do not begin feature work until
`modernization-foundation-final` is public. Do not ship a release candidate
until the applicable physical-device gates pass. A failure is a renderer or
platform defect to fix in the rewrite; it is not a reason to transplant the
exploratory port wholesale.
