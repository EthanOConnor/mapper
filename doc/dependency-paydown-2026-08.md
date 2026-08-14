# Dependency paydown — August 2026

This review advances the production Vello/wgpu product line. The separate Qt
Canvas Painter experiment is not part of this change.

## Upgraded

| Dependency | From | To | Why it matters here |
|---|---:|---:|---|
| CMake | 4.4.0 | 4.4.2 | Takes the current 4.4 fixes and enables schema-12 presets, install-destination diagnostics, and generated per-test preparation targets. |
| Ninja | 1.13.0 | 1.13.2 | Includes the Windows link fix and correct interrupted-build exit status. Official archives replace the lagging PyPI wrapper. |
| vcpkg baseline | 2026-07-13 | 2026-07-29 | Advances the complete native dependency graph as one reviewed, immutable baseline. |
| GDAL | 3.13.1 | 3.13.2 | Adds CMake 4.4 compatibility and fixes `/vsicurl/` size handling, cloud-sync locking, edge-window reads, and integer overflow on huge rasters. |
| SQLite | 3.53.3 | 3.53.4 | Takes the latest fixes on the 3.53 line while retaining the iOS required-reason-API patch. |
| Rust toolchain | floating stable | 1.97.1 | Makes renderer builds reproducible while taking current compiler and standard-library fixes. The crate MSRV remains 1.88. |
| `cxx` family | 1.0.197 | 1.0.199 | Keeps the C++/Rust renderer bridge current as one lockstep family. |
| Cargo lock graph | July 2026 lock | current MSRV-compatible | Refreshes build, proc-macro, WebAssembly, portability, and serialization crates without changing the reviewed Vello/wgpu API boundary. |
| Python bootstrap | 3.13.3 | 3.14.6 | Moves CI and `aqtinstall` to its recommended, frequently tested interpreter and takes the current 3.14 fixes. |
| Windows GPU test runtime | Agility SDK 1.619.0 / July 2025 DXC | Agility SDK 1.619.5 / DXC 1.9.2607 | Takes the retail D3D12 runtime/debug-layer fixes and current HLSL/DXIL correctness fixes while keeping wgpu's required SDK ABI version 619. |
| GitHub Actions | prior patch releases | current stable patches | Updates checkout, Python, Java, and build-provenance actions while retaining immutable SHA pins. |

## Reviewed and retained

- **Qt 6.10.3 for desktop and Android; Qt 6.11.1 for iOS.** Qt 6.11.1 is the
  newest stable release, but its public Windows and Android metadata still
  fails through released `aqtinstall`; Linux, macOS, and iOS archives are
  available. A single cross-platform baseline is more valuable than silently
  giving each desktop a different Qt runtime. The Qt 6.12 Canvas Painter line
  remains a separate experiment.
- **Vello 0.9.0 / wgpu 29.0.4.** Vello 0.9.0 is the newest stable Vello and
  requires `wgpu ^29.0.3`. wgpu 30.0.0 is newer, but adopting it now would
  require carrying a private Vello port across every graphics backend. Keep
  the latest compatible wgpu patch until Vello publishes its wgpu 30 migration.
- **PROJ 9.8.1, ICU 78.3, zstd 1.5.7, Clipper2 2.0.1, Corrosion 0.6.1,
  KDSingleApplication 1.2.1, and cargo-about 0.9.1.** Each is already current.
- **Android API 36, JDK 21, and NDK r27c (27.2.12479018).** Android NDK r29 is
  newer, but Qt's supported-platform guidance says to use the same r27c build
  used for the official Qt libraries to avoid missing-symbol failures.
- **Microsoft WARP 1.0.20.** This is still the newest stable software D3D12
  adapter; the newer 1.65535.20 package is explicitly a preview.

## Adopted capabilities

- Presets now use CMake schema 12 and diagnose absolute install destinations.
- `CMAKE_TEST_BUILD_DEPENDS` exposes `test_prep/<test-name>` targets, making a
  focused edit/test loop build only the selected test and its dependencies.
- CI installs Ninja directly from official per-platform artifacts and verifies
  the publisher-provided SHA-256 and executable version.
- Windows graphics tests verify SHA-256 before executing the pinned WARP,
  retail Agility SDK, and DXC archives.
- Rust is no longer a moving `stable` input: local builds and every hosted lane
  select the same compiler release.

## Follow-up triggers

- Delete the GDAL overlay when an official vcpkg baseline reaches 3.13.2.
- Re-evaluate one Qt 6.11.1 baseline when released public tooling can install
  the complete Windows and Android archive set without credentials or custom
  repository logic.
- Upgrade Vello and wgpu together after a stable Vello release supports wgpu
  30, with renderer parity, surface lifecycle, and physical mobile gates.
