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

## Performance invariant

Mapper is remarkably fast at rendering large raster templates — 500MB TIFFs render
smoothly at 60fps with instant panning and zooming, even on mobile. The existing
pipeline achieves this by loading the image once into RAM and letting QPainter clip
to the viewport during draw.

**Our tiled path must never degrade this experience.** Specifically:

- `drawTemplate()` must NEVER perform I/O (no GDAL reads, no network, no disk)
- Painting must always be instant — draw from a tile cache, nothing else
- Previously-loaded tiles must persist during panning (no blank flashes)
- The existing non-tiled path (GeoTIFF, JPEG, PNG) must be completely untouched

## The solution

A tile cache with background loading, using the project's existing dirty-rect
mechanism for progressive updates.

This is not "WMS support" — it is viewport-aware raster loading for any tiled GDAL
source. WMS, WMTS, TMS, MBTiles, and COG all benefit from the same code path.

## Architecture

```
Existing path (unchanged):
  GeoTIFF/JPEG/PNG → GDAL reads full image → QImage → drawTemplate() paints it

New tiled path:
  WMS XML / MBTiles / COG → GDAL opens, reports block_size
    → loadTemplateFileImpl() stores dataset handle + georef, starts worker thread
    → drawTemplate() draws cached tiles, queues missing tiles
    → worker thread reads tiles from GDAL, posts to UI thread
    → UI thread inserts tile in cache, calls setTemplateAreaDirty()
    → dirty-rect system triggers repaint of just that tile's area
```

### Rendering loop (drawTemplate, UI thread, const, zero I/O)

```
1. applyTemplateTransform(painter)
2. Map viewport to tile grid coordinates (+ 1 tile overscan border)
3. For each tile in the grid:
   if tile_cache has it → painter->drawImage(tile_position, cached_tile)
   else → queue_tile_request(tile_key)   [non-blocking enqueue]
4. Return immediately
```

Drawing only touches the tile cache. Missing tiles appear as transparent gaps
that fill in as the background worker delivers them. Old tiles at old positions
remain visible during panning — no blank flashes.

### Background worker (dedicated thread, serialized GDAL access)

```
Loop (blocks on empty queue):
  Take tile_key from request queue
  QMutexLocker lock(&gdal_mutex)
  Read tile via GDALDatasetRasterIO() → QImage
  Post (tile_key, QImage) to UI thread via QMetaObject::invokeMethod(Qt::QueuedConnection)
```

One worker thread per tiled template. GDAL dataset access serialized by mutex
(GDAL is not thread-safe for concurrent reads on the same dataset). The mutex
is ONLY held during the GDAL read — never on the UI thread.

### Tile arrival (UI thread callback)

```
onTileLoaded(tile_key, tile_image):
  tile_cache.insert(tile_key, tile_image)
  evict oldest tiles if over memory budget
  setTemplateAreaDirty()  // triggers repaint of tile's map-coordinate area
```

`setTemplateAreaDirty()` feeds the project's existing dirty-rect mechanism.
Only the tile's area is repainted — not the whole view.

### Coordinate chain (already exists, we just use it)

```
Map coords (mm on paper)
  ↕  template transform (position, rotation, scale)
Template pixel coords (image pixels)
  ↕  georef→pixel_to_world transform
Projected CRS coords (meters in UTM/etc.)
  ↕  GDAL geotransform (inverse)
Raster pixel coords (column, row)
  ↕  divide by block_size
Tile grid coords (tile_x, tile_y)
```

## File changes

### `gdal_image_reader.h`
- Add `QSize block_size` to `RasterInfo` struct (from POC)

### `gdal_image_reader.cpp`
- In `readRasterInfo()`: query `GDALGetBlockSize()` on first band;
  clear block_size if it equals full raster size (from POC)
- Add `readBlock(QImage* image, QRect pixel_rect, const RasterInfo& info)`:
  reads an arbitrary raster rectangle via `GDALDatasetRasterIO()`. Uses the
  same band layout and format from RasterInfo. Factored from existing `read()`
  to avoid duplication.

### `gdal_template.h`
```cpp
// New members (private):
GDALDatasetH tiled_dataset = nullptr;
GdalImageReader::RasterInfo tiled_raster_info;
QSize tiled_raster_size;            // full virtual raster dimensions
QThread* tile_worker = nullptr;     // background loader thread
QMutex gdal_mutex;                  // serializes GDAL dataset access
QQueue<QPoint> tile_request_queue;  // pending tile grid coords
QMutex queue_mutex;
QWaitCondition queue_condition;     // wakes worker when requests arrive

mutable QHash<quint64, QImage> tile_cache;  // tile grid key → image
mutable QMutex cache_mutex;                  // protects cache reads/writes
QSet<quint64> loading_tiles;                 // in-flight requests (prevents duplicates)

// New methods:
void requestTile(QPoint tile_coord) const;  // enqueue for background load
void onTileLoaded(QPoint tile_coord, QImage tile);  // UI thread callback

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
  start tile_worker thread
  do NOT read pixels — return true
else:
  existing path (read full image)
```

**drawTemplate()** — tiled branch:
```
if tiled_dataset is null:
  TemplateImage::drawTemplate(...)  // existing path, untouched
  return

applyTemplateTransform(painter)
determine visible tile grid range from painter transform + viewport
expand range by 1 tile in each direction (overscan)
for each (tx, ty) in range:
  key = tileKey(tx, ty)
  QMutexLocker lock(&cache_mutex)  [read lock, fast]
  if tile_cache.contains(key):
    painter->drawImage(tileToPixelRect(tx, ty), tile_cache[key])
  else:
    requestTile(QPoint(tx, ty))    [non-blocking enqueue]
```

**unloadTemplateFileImpl()**:
```
signal worker thread to stop
worker->wait()
delete worker
GDALClose(tiled_dataset)
tiled_dataset = nullptr
tile_cache.clear()
TemplateImage::unloadTemplateFileImpl()
```

### `template_image.cpp`

Minimal change from POC: in `updatePosFromGeoreferencing()`, account for image
offset when computing corner pass points. Only needed for the tiled path where
the image origin is not (0,0) in raster space.

## What does NOT change

- `TemplateImage::drawTemplate()` for local files — completely untouched
- `TemplateImage::loadTemplateFileImpl()` — completely untouched
- Template registration/factory — no new template type
- UI — WMS XML files open via existing "Add Template → Open File" workflow
- Build system — no new dependencies (QThread, QMutex are in Qt5Core)
- Other template types (TemplateMap, TemplateTrack, OgrTemplate)

## Performance comparison

| Aspect | Current (500MB TIFF) | Tiled (MBTiles, local) | Tiled (WMS, network) |
|---|---|---|---|
| `drawTemplate()` I/O | None (image in RAM) | None (tile cache only) | None (tile cache only) |
| First load | Image load time | ~50ms (visible tiles) | 1-3s (network), tiles fill in progressively |
| Pan (cached area) | Instant | Instant | Instant |
| Pan (new area) | Instant (full image in RAM) | Tiles fill in ~5-20ms | Tiles fill in progressively |
| Memory | Full image in RAM (500MB) | Tile cache ~25-100MB (bounded) | Tile cache ~25-100MB (bounded) |
| UI thread blocking | Never | Never | Never |

For local tiled sources: near-parity with current experience.
For network sources: progressive loading (same as every web map — gaps fill in).

## Thread safety

- **UI thread** owns the tile cache exclusively for WRITES (via `onTileLoaded`)
- **UI thread** reads the tile cache in `drawTemplate()` under a read lock
- **Worker thread** reads from GDAL under `gdal_mutex` — never touches the cache directly
- **Cross-thread handoff**: `QMetaObject::invokeMethod(Qt::QueuedConnection)` — Qt-native, safe
- **No shared mutable state without locks** — no races possible

GDAL thread safety: one dataset per template, serialized access via `gdal_mutex`.
This is the documented safe usage pattern for GDAL.

## Qt6 forward compatibility

Every API we use is Qt6-safe:
- QImage, QPainter, QTransform, QRectF, QPoint, QSize — unchanged in Qt6
- QThread, QMutex, QWaitCondition, QQueue — unchanged in Qt6
- QMetaObject::invokeMethod with Qt::QueuedConnection — unchanged in Qt6
- QHash — unchanged in Qt6
- No QStringRef, QMatrix, QRegExp, or other removed APIs
- GDAL C API is Qt-independent

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

### Caching layers
1. **GDAL internal**: WMS driver caches tiles to disk automatically
2. **Our tile cache**: QImage tiles in memory, avoids repeated GDAL calls during repaints
3. **Map view cache**: dirty-rect compositing avoids re-calling drawTemplate() at all

Three layers, each serving a different purpose. They compose cleanly.

## Issues resolved

| Issue | How |
|---|---|
| #84 WMS support | Open GDAL WMS XML file as template → async tile loading |
| #1945 MBTiles | Open .mbtiles file as template → same async tile loading |
| #2470 Large GeoTIFF | Cloud Optimized GeoTIFFs with block structure also benefit |

## User workflow (PR 1, no UI changes)

1. Create a GDAL WMS XML description file (or obtain an .mbtiles file)
2. In Mapper: Templates → Open template... → select the file
3. GDAL opens it; Mapper detects block structure; tiles load progressively

## PR series

| PR | Scope | Dependencies |
|---|---|---|
| **1** | Tile cache + async background loading + dirty-rect integration | None |
| **2** | "Add web template" dialog (URL → GDAL XML generation) | PR 1 |
| **3** | Resolution/zoom level management (GDAL overviews) | PR 1 |
| **4** | Predictive loading, bandwidth prioritization | PR 1 |

PR 1 is the engine with production-quality rendering. PRs 2-4 are UX refinements.

## Testing strategy

- Local VRT (GDAL Virtual Raster) with configured block size as test fixture
- Verify: tiled source detected, full raster NOT allocated
- Verify: drawTemplate() with no cached tiles returns immediately (no blocking)
- Verify: tiles arrive asynchronously and trigger repaint
- Verify: non-tiled source (GeoTIFF) still uses existing path unchanged
- Verify: unload/reload cycle cleans up dataset handle and stops worker thread
- Verify: tile cache respects memory limit
- No network access required for tests
