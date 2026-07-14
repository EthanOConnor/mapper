# Windows physical-print acceptance

Run this gate on every release candidate that changes Qt, rendering, print
layout, or packaging. A hosted runner cannot substitute for a real Windows
printer driver and device.

## Setup

- Install the packaged release candidate on Windows 11.
- Record the commit, Windows build, printer model, driver name/version, paper
  size, and whether the driver uses a vendor or Microsoft class driver.
- Use `examples/complete map.omap`, `examples/overprinting.omap`, and
  `test/data/issue-513-coords-outside-printable.omap`.
- Test print preview, Microsoft Print to PDF, and one physical printer.

## Checks

1. Print at the map's configured scale and verify a known 100 mm map distance
   measures 100 mm within 0.2 mm on both axes.
2. Verify the printable-area origin and page margins against preview; no
   constant horizontal or vertical offset is allowed.
3. Inspect thin curves, line patterns, text, clipping, transparency, and
   overprint output for coordinate stair-stepping, dropped marks, or unexpected
   full-page rasterization.
4. Change paper size, orientation, resolution, copies, and a vendor-specific
   property. Reopen the dialog and verify the selected settings survive and the
   produced page matches them.
5. Export the same page to PDF and compare geometry and color ordering. Driver
   color-management differences are recorded, not silently accepted as Mapper
   geometry differences.

## Evidence

Attach the generated PDF, a 600 dpi scan or calibrated photograph of the
physical page, and this completed record to the release candidate:

```text
commit:
Windows build:
Mapper package:
printer and driver:
paper and orientation:
configured resolution:
100 mm measurement, X / Y:
preview-to-page offset:
vendor settings retained:
visual anomalies:
result: PASS / FAIL
tester and date:
```

The gate passes only when the measurement, placement, settings, and visual
checks pass on a physical device. A PDF-only run is useful diagnostic evidence
but is not release acceptance.
