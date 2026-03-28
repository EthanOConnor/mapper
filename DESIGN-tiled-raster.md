# Design: Viewport-Aware Block Loading for GDAL Raster Sources

**Target issues:** #84 (WMS support, v3.0 milestone, 46 comments), #1945 (MBTiles)
**Builds on:** PR #1816 (WMS POC by Kai Pastor, draft)
**Approach:** Extend GdalTemplate to load raster data on-demand for tiled/large sources

## The problem

Mapper currently loads the entire raster into a QImage during `loadTemplateFileImpl()`.
This works for GeoTIFFs and local images, but breaks for GDAL sources with enormous
virtual dimensions — WMS, WMTS, TMS, MBTiles, Cloud Optimized GeoTIFF. GDAL reports
these as multi-million-pixel rasters; allocating them crashes the process.

The maintainer identified this explicitly:
> "The problem is probably the same as for WMS: there is not a particular reasonable
> size in pixel, as assumed by Mapper. Mapper would need to request data for a
> particular area of interest." — Kai Pastor, #1945

## The solution

When a GDAL source reports meaningful block sizes (indicating a tiled source), keep
the GDAL dataset handle open and read only the raster region visible in the current
viewport. GDAL handles HTTP fetching, tile management, and internal caching for
WMS/WMTS/TMS sources. Mapper just asks for pixels.

This is not "WMS support" — it is viewport-aware raster loading for any tiled GDAL
source. WMS, WMTS, TMS, MBTiles, and COG all benefit from the same code path.

## Architecture

```
Existing path (unchanged):
  GeoTIFF/JPEG/PNG → GDAL reads full image → QImage → drawTemplate() paints it

New tiled path:
  WMS XML / MBTiles / COG → GDAL opens, reports block_size
    → loadTemplateFileImpl() stores dataset handle + georef (no full read)
    → drawTemplate() maps clip_rect to raster pixels
    → GDALDatasetRasterIO() reads visible rectangle
    → paint from viewport cache
```

### Coordinate chain (already exists, we just use it)

```
Map coords (mm on paper)
  ↕  template transform (position, rotation, scale)
Template pixel coords (image pixels)
  ↕  georef→pixel_to_world transform
Projected CRS coords (meters in UTM/etc.)
  ↕  GDAL geotransform
Raster pixel coords (column, row)
```

`drawTemplate()` receives a `QPainter` already positioned in template pixel space.
We invert the geotransform to get from viewport bounds to raster pixel bounds.

## File changes

### `gdal_image_reader.h`
- Add `QSize block_size` to `RasterInfo` struct (from POC)

### `gdal_image_reader.cpp`
- In `readRasterInfo()`: query `GDALGetBlockSize()` on first band;
  clear block_size if it equals full raster size (from POC)
- Add `readBlock(QImage* image, QRect pixel_rect)`: reads an arbitrary
  raster rectangle via `GDALDatasetRasterIO()`. Uses the same band
  layout and format from `readRasterInfo()`. Factored from existing
  `read()` to avoid duplication.

### `gdal_template.h`
```cpp
// New members (private):
GDALDatasetH tiled_dataset = nullptr;
GdalImageReader::RasterInfo tiled_raster_info;
QSize tiled_raster_size;  // full virtual raster dimensions
mutable QImage viewport_cache;
mutable QRect  viewport_cache_rect;  // raster pixel rect of cached region

// New overrides:
void drawTemplate(...) const override;
QRectF getTemplateExtent() const override;
void unloadTemplateFileImpl() override;
```

### `gdal_template.cpp`

**loadTemplateFileImpl()** — tiled branch:
```
if raster_info.block_size is valid:
  keep dataset handle open (tiled_dataset)
  store raster_info and full raster size
  set up georeferencing from GDAL geotransform (reuse existing code)
  do NOT read pixels — return true
else:
  existing path (read full image)
```

**drawTemplate()** — tiled branch:
```
if tiled_dataset is null:
  TemplateImage::drawTemplate(...)  // existing path
  return

apply template transform (applyTemplateTransform)
map viewport clip_rect → template pixel bounds (invert painter transform)
map template pixels → raster pixels (invert geotransform)
clamp to raster bounds
if cached region covers needed area:
  draw from cache
else:
  expand cache rect (add margin for smooth panning)
  read expanded rect via GDALDatasetRasterIO
  store in viewport_cache
  draw
```

**unloadTemplateFileImpl()**:
```
GDALClose(tiled_dataset)
tiled_dataset = nullptr
viewport_cache = QImage()
TemplateImage::unloadTemplateFileImpl()
```

**Destructor**: close tiled_dataset if open.

### `template_image.cpp`

Minimal change from POC: in `updatePosFromGeoreferencing()`, account for image
offset when computing corner pass points. Only affects tiled sources.

## What does NOT change

- TemplateImage for local files (GeoTIFF, JPEG, PNG) — completely untouched
- Template registration/factory — no new template type; GdalTemplate handles both paths
- UI — WMS XML files open via existing "Add Template → Open File" workflow
- Build system — no new dependencies
- Other template types (TemplateMap, TemplateTrack, OgrTemplate)

## Qt6 forward compatibility

Every API we use is Qt6-safe:
- QImage, QPainter, QTransform, QRectF, QPoint, QSize — unchanged in Qt6
- No QStringRef (removed in Qt6)
- No QMatrix (removed in Qt6)
- No QRegExp (replaced in Qt6)
- mutable cache members are standard C++ (not Qt-version-dependent)
- GDAL C API is Qt-independent

The `#if QT_VERSION` guards in `TemplateImage::drawTemplate()` (for QTBUG-70752
workaround) already handle Qt6. Our override for tiled sources sidesteps these
entirely — we don't need print-engine workarounds for on-screen tile rendering.

When the project moves to Qt6 (#2483), our code will compile unchanged.

## GDAL considerations

### Driver availability
The WMS/WMTS/TMS drivers are built into GDAL by default but can be disabled at
compile time. MBTiles requires SQLite support (almost always present). Our code
doesn't assume any specific driver — it checks `block_size` at runtime. If a source
doesn't have block semantics, the existing load-all path handles it.

### Version compatibility
- `GDALDatasetRasterIO()` with offset: available since GDAL 1.x
- `GDALGetBlockSize()`: available since GDAL 1.x
- No GDAL version bump needed
- The existing `#if GDAL_VERSION_MAJOR >= 3` guards for axis mapping are unaffected

### Caching
GDAL's WMS driver has built-in tile caching (disk and memory). Our viewport cache
on top adds a second layer — avoiding repeated GDAL calls during QPainter repaints
of the same view. These compose cleanly; GDAL handles HTTP-level caching, we handle
render-level caching.

### Potential follow-up: GDAL CMake improvements
- Add explicit minimum version: `find_package(GDAL 2.0 MODULE REQUIRED)`
- Add driver availability diagnostic at configure time
- These are separate PRs — they improve the project's GDAL integration broadly

## Issues resolved

| Issue | How |
|---|---|
| #84 WMS support | Open GDAL WMS XML file as template → viewport-aware loading |
| #1945 MBTiles | Open .mbtiles file as template → same viewport-aware loading |
| #2470 Large GeoTIFF | Cloud Optimized GeoTIFFs with block structure also benefit |

## User workflow (PR 1, no UI changes)

1. Create a GDAL WMS XML description file (or obtain an .mbtiles file)
2. In Mapper: Templates → Open template... → select the file
3. GDAL opens it; Mapper detects block structure; tiles load as you pan/zoom

## PR series

| PR | Scope | Dependencies |
|---|---|---|
| **1** | Viewport-aware block loading in GdalTemplate | None |
| **2** | "Add web template" dialog (URL → GDAL XML generation) | PR 1 |
| **3** | Async tile loading (background thread + signal on completion) | PR 1 |
| **4** | Resolution/zoom level management | PR 1 |

PR 1 is the engine. PRs 2-4 are UX polish that build on it independently.

## Testing strategy

- Local VRT (GDAL Virtual Raster) with configured block size as test fixture
- Verify: tiled source detected, viewport-aware read called, full raster NOT allocated
- Verify: non-tiled source (GeoTIFF) still uses existing path unchanged
- Verify: unload/reload cycle cleans up dataset handle
- No network access required for tests
