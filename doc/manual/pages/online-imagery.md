---
title: Online imagery
keywords: Templates, Imagery, OIC, XYZ, TMS, ArcGIS
parent: Templates and Data
nav_order: 0.2
last_modified_date: 16 July 2026
---

Online imagery is a georeferenced raster template which loads only the tiles
needed for the current view or output. The map must be georeferenced, but it
does not need to be saved before imagery is added.

Open **Templates → Add online imagery…**, or use **Add template… → Add online
imagery…** in the template setup window.

## Choosing a source

The source browser lists sources from installed OpenOrienteering Imagery
Catalog (OIC) files. Search matches source names, descriptions, identifiers and
categories. Unsupported catalog entries remain visible with an explanation.

Selecting a source shows its catalog revision, imagery dates, coordinate
reference system, zoom range, request hosts, attribution and terms. The
template name may be changed without changing the catalog source identity.

When the source is added, Mapper stores a complete, checksummed source snapshot
inside the map. Updating or removing the installed catalog does not silently
change an existing map.

## Entering a URL

Choose **Enter a tile or service URL…** to add a source which is not in a
catalog.

Direct tiled sources use an HTTP or HTTPS URL containing all three
`{z}`, `{x}` and `{y}` placeholders. The documented `${z}`, `${x}` and `${y}`
spellings are accepted and normalized. Advanced settings make the row scheme
(XYZ or TMS), zoom range, 256 or 512 pixel tile size, HTTP Referer, empty-tile
status codes and attribution explicit.

Cached ArcGIS MapServer and ImageServer links are checked by reading their
published `f=pjson` metadata. Mapper derives the tile grid, levels, origin,
coordinate reference system, format and endpoint from the service rather than
assuming Web Mercator or a fixed zoom range.

WMS, WMTS and remote GeoTIFF links are recognized but are not currently
executed by the native tiled raster template. Mapper reports them as
unsupported instead of guessing an incompatible source configuration.

## Catalogs

Choose **Manage catalogs…** to import, update or remove OIC catalogs.

Before installation, Mapper shows:

- catalog identity, revision, publisher, origin and document SHA-256;
- added, removed, operationally changed and metadata-only source counts;
- usable, invalid and unsupported source counts;
- request hosts and duplicate-source counts;
- warnings for downgrade, republished revisions, HTTP, local-network
  endpoints, registration corrections and credential-like query parameters.

Catalog updates use ETag and Last-Modified validators when available. An
interrupted update cannot replace the current catalog with a partial file, and
the previous snapshot is retained for recovery.

## Network privacy and credentials

Cookies and HTTP authentication are not used for imagery requests. Redirects,
response sizes and timeouts are bounded. HTTPS-to-HTTP redirects are rejected.

Private and local-network origins require an explicit permission stored only
on the current installation. A catalog or map file cannot grant this
permission. This includes private, shared, link-local, documentation,
benchmarking and other special-purpose address ranges, whether written
directly in a URL or returned by DNS.

Choose **Templates → Imagery network permissions…** to review or revoke saved
permissions. The same dialog lists origins from requests Mapper blocked after
DNS resolution; approving one is always a deliberate user action. Merely
opening a map may add a blocked origin to the review list, but never grants it
network access or opens a permission prompt.

Mapper's hostname preflight is defense in depth: Qt's network stack performs
its own connection-time resolution, so operating-system firewall and network
policy remain the hard isolation boundary.

URLs containing token-like query parameters are not added to a recent-sources
list. Mapper warns before embedding the complete endpoint in the map. Anyone
who can read that map file may be able to read the endpoint, including its
query parameters.

## Screen extent and zooming

On screen, imagery is clipped to the map's drawn-object extent with 20 percent
padding on every side, and tiles wholly outside that area are not requested.
This keeps a broad service coverage area from changing **Zoom entire map** or
using cache space far beyond the working map. A map with no drawn objects falls
back to the source's published tile extent.

During zooming, Mapper keeps the last complete imagery scene visible while the
replacement level is downloaded, decoded and admitted to the renderer. The new
level replaces it as one scene, avoiding a white intermediate frame or a
partially updated tile grid.

## Offline use

Enable **Templates → Work offline for imagery** to prevent network access and
use only tiles already present in the local HTTP cache. Turning offline mode
off allows missing tiles to be requested again.

Referer-dependent imagery is not retained in the shared offline cache because
serving it later without the original request context can be incorrect.

## Printing and export

Screen display may temporarily use a coarser cached parent tile while a sharper
tile loads. Print, PDF, image and KML/KMZ output do not use provisional parents.
Mapper prepares the exact requested source level before opening the output
paint engine. A missing or permanently failed tile stops exact output with an
error instead of silently producing an incomplete map.

Large translucent output is prepared as bounded, non-overlapping atlas chunks.
Chunk sizes account for both source and reprojected output geometry, and
neighbor pixels are sampled before each chunk is cropped to its own coverage.
This retains seam-free alpha blending without requiring one page-sized raster
allocation. Exact preparation also reserves the renderer snapshot memory it
will need before reporting that output is ready.

Exact-imagery preparation and multi-tile KML/KMZ export can be canceled. A
user cancellation is reported as canceled rather than as a rendering failure.
Incomplete staged PDF, image and KMZ files are discarded. Image exports with
a world file use a destination lock, recovery journal and old-image backup; if
the two-file commit is interrupted, Mapper restores the matching old pair or
finishes cleanup before the next export. Direct KML image sidecars are
published through a unique asset directory; unreferenced directories left by
a process interruption are recovered on the next export. Interrupted KML/KMZ
staging files are scoped to their absolute destination and safely removed when
that destination is exported again.
