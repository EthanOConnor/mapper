# Renderer interaction parity

This is the small manual gate for comparing the rewritten retained renderer
with a named historical renderer. It is not a product automation framework.
The hook is inert unless `OOM_RENDER_VALIDATION_DRIVER` is set.

## Trace

At a 1266 x 919 logical-pixel viewport and DPR 2, the driver waits for the map
to settle and then performs the following deterministic input sequence at a
nominal 16 ms cadence:

1. 60 pan updates ending at `(260, -140)` pixels, then commit.
2. 31 anchored pinch updates from 1x to 2x, then commit.
3. An anchored two-step zoom in and 1.5-step zoom out.
4. 40 pan updates ending at `(-220, 110)` pixels, then commit.

The start, every committed camera state, and the final state may be captured as
lossless backend output by setting `OOM_RENDER_VALIDATION_CAPTURE_DIR`. Native
render CPU timings are enabled with `OOM_RENDER_TIMING`; the default summary
window is 120 presented frames and can be changed with
`OOM_RENDER_TIMING_WINDOW`. `OOM_RENDER_VALIDATION_EXIT` closes any file-load
warning dialog, allows its callback to unwind, and then quits the application
so a noninteractive run terminates safely.

Example Release run:

```sh
OOM_RENDER_VALIDATION_DRIVER=1 \
OOM_RENDER_VALIDATION_CAPTURE_DIR=/tmp/mapper-trace \
OOM_RENDER_VALIDATION_EXIT=1 \
OOM_RENDER_TIMING=1 \
build/release-macos/src/Mapper.app/Contents/MacOS/Mapper \
examples/complete\ map.omap
```

## 2026-07-14 evidence

The comparison used an Apple M3 Max running macOS 27.0 build 26A5378j, a
Release build, Qt 6.11.1, Metal, a 1266 x 919 logical-pixel viewport, and DPR 2
(2532 x 1838 physical pixels). The historical line was
`full-speed-ahead@74c364569059f8e83f1f9e2623671ff9fe2b9fff`. The rewrite was
`main@f85b0f4e741c03b991095062ba325444430e708f` plus the parity instrumentation
recorded by the commit containing this report.

Private paths are deliberately omitted. The exact tested assets are identified
by stable corpus id and SHA-256:

| Corpus id | SHA-256 | Character |
|---|---|---|
| `complete-map` | `98452b44e8b03cb09ce0a8ed7acc204365f4ea8bc216debb2c432b40ae82a915` | Public mixed symbols, text, patterns |
| `fishtrap-current` | `c67e1ef577feccda1e66bca4f6b07ed888671496f99842e4a5c8f20a984e4811` | 8.7 MiB real club map, 62,746 retained commands |
| `kelsey-dense-contours` | `844d0546d4e5d180ea909482364b6becf1e0912115d3b93bd9afafb115919b75` | Dense contour geometry |

Both products ran the trace above from the same fresh settings directory.
These are the comparable render-thread CPU measurements from the first 120
presented native frames; they do not include the GUI cadence timer or an
offscreen readback:

| Corpus id | Rewrite average / p95 | `full-speed-ahead` average / p95 | Result |
|---|---:|---:|---|
| `complete-map` | 0.909 / 1.156 ms | 1.490 / 2.793 ms | Rewrite has lower mean and tail |
| `fishtrap-current` | 2.338 / 3.011 ms | 2.182 / 4.602 ms | Similar mean; rewrite has a materially tighter tail |
| `kelsey-dense-contours` | 0.956 / 1.849 ms | 1.826 / 3.746 ms | Rewrite has lower mean and tail |

The rewrite also passed the stricter synchronized GPU-render-and-CPU-readback
benchmark at 1024 x 768. This is diagnostic headroom evidence, not a substitute
for native presentation latency:

| Corpus id | Commands | p50 / p95 / maximum |
|---|---:|---:|
| `complete-map` | 10,736 | 4.519 / 5.911 / 7.110 ms |
| `fishtrap-current` | 62,746 | 14.584 / 15.999 / 18.398 ms |
| `kelsey-dense-contours` | 1,415 | 4.791 / 7.355 / 7.690 ms |

### Visual and interaction assessment

- All three traces reached the same committed zoom values and final camera
  states in both products. No blank frame, seam, incomplete revision, or anchor
  jump was observed in the sampled states.
- OS-level `full-speed-ahead` window captures and lossless rewrite captures
  showed the same map geometry, text, line patterns, colors, and ordering. The
  rewrite was at least as crisp at physical resolution; no graphic-quality
  regression was found.
- The native 120-frame measurements remain well inside the 8.33 ms dense-map
  render-thread budget. The lower p95 values and absence of duplicate
  submissions support smoother input response in the rewrite. During this run,
  the trace exposed and removed an unnecessary old-frame submission caused by
  assigning an unchanged canvas background before every new frame.
- `QWidget::grab()` cannot capture the historical native Metal child surface;
  its all-black images are not render failures. Historical visual evidence was
  therefore captured at the OS window boundary. The rewrite's lossless capture
  renders its immutable current frame through the same Vello encoder.

The captured phase states received direct visual review. A release candidate
should still be watched continuously by a club mapper on representative
hardware; still captures and CPU timing cannot replace a person's judgment of
trackpad feel or display pacing.
