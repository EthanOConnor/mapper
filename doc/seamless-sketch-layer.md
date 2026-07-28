# Seamless sketch layer

## Product behavior

Mapper has one logical **Sketch** layer per map. Activating **Sketch** creates
that layer only when it does not already exist. A mapper can pan or zoom
anywhere and continue drawing without selecting or creating another canvas.

The interaction is optimized for field annotation:

- arbitrary palette colors, including alpha;
- fine (0.18 mm), medium (0.35 mm), and broad (0.70 mm) round pens;
- touch/stylus freehand input with sub-pixel geometric simplification;
- a 1.5 mm stroke eraser that removes every sketch stroke it crosses;
- ordinary map undo and redo.

Sketches are helper content: visible while editing, excluded from production
print/export rendering, and protected from accidental manipulation by ordinary
object-editing tools.

## Data model

The sketch is vector data, not a raster template.

- One normal UUID-bearing `MapPart` named `Sketch` is the logical layer.
- Each gesture is one normal UUID-bearing `PathObject` in map coordinates.
- Styles are lazily reused `SketchSymbol` instances.
- A sketch symbol stores its ARGB color and physical paper width directly.
- Dense pointer samples are simplified to a 0.65-screen-pixel tolerance before
  the path object is committed.

There is no fixed canvas, pixel grid, tile boundary, auxiliary PNG, or memory
allocation proportional to the geographic extent. Resolution is limited only
by the map coordinate model and renderer, so strokes remain crisp at every
zoom.

## Persistence and connected editing

Sketch parts, symbols, and objects use the existing OMAP UUID serialization.
The root sketch-symbol marker is `kind="mapper-sketch-v1"`; older Mapper builds
ignore the unfamiliar sketch payload without losing the rest of the map.

This shape deliberately aligns with Map Hub connected editing:

- first use sends a `part.put`;
- each newly used color/width combination sends one `symbol.put`;
- each completed stroke sends one compact `object.put`;
- erasing sends ordinary `object.delete` operations;
- snapshots and checkpoints contain the same native OMAP data.

No file diff, image upload, untracked color-table mutation, or external
template asset is required.

## Compatibility

Existing `Draft @ … .png` raster templates remain loadable and visible. They
are not automatically vectorized because doing so would invent geometry and
could change their appearance. New field sketches use the seamless layer.

This implementation branch is based on the Luther Burbank iOS build so it can
be field-tested immediately. Its commits are isolated for later rebase onto
current product `main`.
