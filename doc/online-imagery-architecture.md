# Online imagery architecture

This feature intentionally uses a native tiled raster path rather than
regenerating GDAL WMS XML sidecars.

GDAL remains the right abstraction for local geospatial files, broad raster
format support and export. Current GDAL releases also offer thread-safe raster
dataset wrappers for read-only access. Those capabilities do not provide the
application contracts needed here: view-driven request cancellation, source
fairness, provisional adjacent-level tiles, alpha-seam prevention, renderer resource
admission, exact-output preflight, catalog identity or installation-local
network permissions.

References:

- [GDAL WMS driver](https://gdal.org/en/stable/drivers/raster/wms.html)
- [GDAL WMTS driver](https://gdal.org/en/stable/drivers/raster/wmts.html)
- [GDAL multithreading guidance](https://gdal.org/en/stable/user/multithreading.html)
- [GDAL RFC 101 thread-safe raster datasets](https://gdal.org/en/stable/development/rfc/rfc101_raster_dataset_threadsafety.html)
- [Qt 6 QNetworkAccessManager](https://doc.qt.io/qt-6/qnetworkaccessmanager.html)
- [Qt 6 QHostAddress classification](https://doc.qt.io/qt-6/qhostaddress.html)
- [IANA IPv4 special-purpose registry](https://www.iana.org/assignments/iana-ipv4-special-registry/iana-ipv4-special-registry.xhtml)
- [IANA IPv6 special-purpose registry](https://www.iana.org/assignments/iana-ipv6-special-registry/iana-ipv6-special-registry.xhtml)
- [OGC Two Dimensional Tile Matrix Set](https://docs.ogc.org/is/17-083r4/17-083r4.pdf)
- [OGC API - Tiles - Part 1: Core](https://docs.ogc.org/is/20-057/20-057.pdf)

## Layering

```text
OIC/manual/ArcGIS definitions
          |
          v
ResolvedImagerySource -> checksummed embedded snapshot
          |
          v
OnlineRasterTemplate
   | requests                    | immutable render descriptions
   v                             v
TileNetworkManager          TemplateLayerPlanner
   |                             |
   v                             v
bounded HTTP/decode         Qt reference / Vello screen backend
```

Installed catalogs are an input index, not a runtime dependency of a map.
`ImageryCatalogRepository` publishes immutable snapshots and stable
`catalog-id/source-id/catalog-sha256` handles. Selecting a source copies its
resolved definition into an `ImagerySourceSnapshot`; later catalog updates do
not mutate the map.

## Catalog durability

Catalog bytes are immutable and addressed by document SHA-256:

```text
imagery-catalogs/<catalog-id-hash>/
  current.json
  snapshots/<document-sha256>/
    catalog.oic
    snapshot.json
```

`QLockFile` serializes writers across Mapper processes. A `QSaveFile` update of
`current.json` is the only activation step, so catalog bytes and transport
metadata cannot come from different revisions after a crash. The previous
snapshot is retained and used if the active snapshot is damaged. Legacy
mapper-coc `catalog.oic`/`state.json` installations are read and migrated on
the next write.

## HTTP ownership and policy

One application-scoped `QNetworkAccessManager` and `QNetworkDiskCache` live on
a dedicated event-loop thread. Requests are bounded globally, per host and per
source, and are owner-fair before priority and distance ordering.

The manager performs asynchronous DNS checks before contacting a destination.
Literal and resolved addresses share one conservative public-destination
predicate. It supplements Qt's broad `isGlobal()` category with the IANA
special-purpose ranges, including RFC 1918, ULA, shared, benchmarking,
documentation and reserved space. IPv6 authorities are canonicalized through
`QUrl`, preserving brackets and exact permission scope. Cookies, embedded
credentials and HTTP authentication are disabled. Every redirect is
revalidated; HTTPS downgrade and unapproved private destinations are rejected.
Response size, decompression, first-byte, transfer and absolute time limits are
enforced while streaming.

The DNS check deliberately has a narrow claim. `QNetworkAccessManager`
resolves the connection independently, so Mapper shortens but cannot eliminate
the rebinding interval without replacing Qt's TLS, HTTP and cache stack.
Endpoint trust and operating-system network policy remain the hard security
boundary.

Tile images use the shared disk cache. Catalog and service JSON uses
conditional requests but bypasses the tile cache so HTTP 304 remains visible
to the catalog repository. Empty-tile cache keys retain only a fixed-size
digest of the URL, Referer and representation policy. Offline and
private-permission transitions advance facade generations which are checked
again immediately before queued results reach the UI, closing worker/event
ordering races while still allowing genuine cache-only successes.
Private-origin approvals are stored in local settings and can only be created
by explicit UI action.

## Rendering

Opaque tiles become direct image-to-map primitives and can remain independent.
This gives the screen backend granular resource admission and avoids building
a viewport-sized CPU mosaic.

Independent translucent tiles would blend their shared edges more than once.
A translucent screen window is therefore composed once into a retained
premultiplied atlas. Exact output divides large translucent extents into
non-overlapping, tile-aligned atlas chunks. Chunk subdivision accounts for both
source pixels and the projected destination geometry. For cross-CRS output,
each worker reads a true one-pixel strip from adjacent chunks before bilinear
resampling, then masks the result back to its half-open core domain. The
neighbor pixels are filter support only, preventing both transparent sampling
seams and double-alpha overlap. Bounded workers build adaptive inverse-warp
grids and resample those chunks off the UI thread.

The screen may use an exact cropped parent tile as a provisional fallback.
Output sessions pin the requested zoom and derived resources and reject
provisional imagery. Preparation finishes before a printer/PDF/image/KMZ paint
engine is opened, so cancellation or missing exact imagery cannot leave a
nominally successful partial export.

Screen planning intersects the visible view with the drawn map-object extent
padded by 20 percent per side. That intersection bounds tile scheduling and is
also emitted as a backend-neutral raster-layer clip, so a coarse edge tile
cannot visually expand the working extent. Exact output remains governed by
the requested output rectangle. Before frame planning, the viewport consumes
any pending template render-context update, so tile selection and scene
construction use the same view generation. Reprojected patch geometry is
scale-dependent and is therefore rebuilt for the current scale; no raster
RenderIR is carried across a zoom transition. Cached parents provide zoom-in
coverage; cached descendants provide zoom-out coverage. Descendants retain
their already-resident immutable pixels but receive fresh current-scale patch
geometry. Together with the already-requested overscan ring, this makes the
handoff continuous without violating the geometry invariant.

File output is committed transactionally. PDF and standalone image writers
target `QSaveFile`. An image plus world-file export stages both files under a
per-destination process lock; a bounded journal and sibling image backup make
the unavoidable two-file commit recoverable. Rollback restores the world file
before republishing its matching image, so an interruption never exposes old
pixels with new georeferencing.

KMZ archives are completed in sibling staging files before the selected
destination is atomically replaced. Direct KML sidecars are written into a
unique, destination-scoped exporter-owned asset directory and the staged KML
document is published last. A per-destination process lock serializes writers.
Staging filenames include the SHA-256 of the absolute destination; while
holding that lock, the next export removes only matching regular, non-symlink
staging files left by an interrupted writer. The published KML identifies the
live asset directory, so unreferenced directories for that destination are
likewise recovered without touching another KML document's assets.

## Memory and cancellation

Network bodies, DNS waiters, decode jobs, completion delivery, decoded tiles
and derived atlases have separate bounded queues and working-set checks.
Retained raster memory is budgeted across all online templates. Accounting
leases follow shared pixels into queued atlas workers and immutable renderer
snapshots, so cache eviction never reports memory as released while another
owner still holds it. Once exact translucent atlases are complete, their
source-tile pins are released and renderer snapshot memory is reserved before
preflight reports Ready. Admission uses application-wide least-recently-used
eviction before applying backpressure, continuing past externally pinned
entries when reclaimable cache entries remain. View generations remove
superseded network, decode and warp work from their bounded queues. Cached
source tiles survive pans and map georeferencing changes; only derived
map-space atlases are invalidated.

Empty tiles use a compact typed cache state rather than a full transparent
image. Retry state records terminal and policy failures per equivalent
endpoint: transient failures rotate without poisoning a backup, while
approving any previously blocked endpoint revives affected tiles. Offline
misses wait for offline mode to end instead of forming a retry loop.
