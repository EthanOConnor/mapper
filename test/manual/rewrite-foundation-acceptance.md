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

## Release-candidate decisions still requiring people and hardware

These are deliberately small product checks, not missing automation machinery:

1. On Windows 11, complete `windows-print-acceptance.md` with one physical
   printer, a measured 100 mm map distance, retained driver properties, and a
   scan or calibrated photograph.
2. Have a club mapper run the same three maps in `main@f106ee2d` and
   `full-speed-ahead@74c36456` on representative hardware. Record whether pan,
   pinch, cursor-anchored zoom, text and pattern stability, crispness, display
   pacing, and input latency are at least as good in the rewrite.

Do not describe the rewritten foundation as superior in practice, or begin
feature work on it, until both verdicts are recorded as passes. A failure is a
renderer or print defect to fix in the rewrite; it is not a reason to transplant
the exploratory port wholesale.
