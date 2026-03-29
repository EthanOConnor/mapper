# OSM Vector Import — Design Document

## Status: Proposal (2026-03-28)

## Goal

Add a streamlined workflow for importing OpenStreetMap vector data as
properly symbolized orienteering map objects. The user provides their map
extent; we fetch the relevant OSM data, apply existing CRT symbol
mappings, and add the objects to the map.

## What Already Exists

Mapper has nearly all the pieces. The gap is small:

| Component | Status | Location |
|-----------|--------|----------|
| OSM tag -> ISOM/ISSprOM mapping rules | Ships with mapper | `symbol sets/OSM-ISOM 2017-2.crt` (129 rules) |
| OSM tag -> ISSprOM mapping rules | Ships with mapper | `symbol sets/OSM-ISSprOM 2019.crt` (115 rules) |
| GDAL OSM driver configuration | Ships with mapper | `src/gdal/mapper-osmconf.ini` |
| OGR vector import with CRT application | Complete | `src/gdal/ogr_file_format.cpp` (~2400 lines) |
| Symbol rule engine (CRT evaluation) | Complete | `src/core/objects/symbol_rule_set.cpp` |
| Symbol replacement dialog | Complete | `src/gui/symbols/symbol_replacement_dialog.cpp` |
| **Overpass API client** | **Missing** | — |
| **Streamlined fetch-import UI** | **Missing** | — |

The current workflow requires users to manually download a .osm or .pbf
file (e.g. from Geofabrik or Overpass Turbo), then File > Import. This
feature automates the download step and streamlines the import.

## Architecture

```
User: Templates menu > "Import OSM data..."
                    ↓
    ┌─────────────────────────────┐
    │   OsmImportDialog           │
    │   - Map extent shown        │
    │   - Symbol set dropdown     │
    │     (ISOM 2017-2/ISSprOM)   │
    │   - [Fetch & Import]        │
    └──────────┬──────────────────┘
               ↓
    ┌──────────────────────────────┐
    │   OverpassClient             │
    │   - Builds query from CRT    │
    │     tag patterns + osmconf   │
    │   - Fetches via Overpass API │
    │   - Saves to temp .osm file  │
    └──────────┬───────────────────┘
               ↓
    Existing OGR import pipeline
    (ogr_file_format.cpp + mapper-osmconf.ini)
               ↓
    CRT rules applied via SymbolRuleSet
               ↓
    Objects added to map with IOF symbols
               ↓
    Optional: SymbolReplacementDialog for review
```

## Design Decisions

### 1. Overpass API, not raw planet/extract downloads

- Overpass supports bounding-box + tag-filtered queries
- Returns only what we need (orienteering-relevant features)
- Standard API, no account/key required
- Widely used by OSM editors and analysis tools

### 2. Build queries from CRT tag patterns

Rather than hardcoding which OSM tags to fetch, parse the CRT file to
extract the set of tags referenced in rules. Union with the tags in
mapper-osmconf.ini. This makes the query automatically stay in sync
with mapping updates.

Example: if CRT has `natural = water`, `highway = path`, `building = yes`,
the Overpass query filters to features matching any of those tags.

### 3. Reuse OGR import pipeline entirely

The fetched .osm XML is a standard OSM file. Feed it directly to the
existing OGR import path (`OgrFileImport`). This means:
- Coordinate transforms already handled
- mapper-osmconf.ini tag extraction already works
- CRT rule application already works
- No new symbol creation or object conversion code needed

### 4. No new symbol mapping format

Use the existing CRT file format. Any improvements to tag coverage
should be CRT file updates, not code changes. CRT files are data,
not code — they can be updated independently of the import feature.

### 5. Qt6-forward APIs only

- Use QStringView instead of QStringRef where touching existing code
- QNetworkAccessManager for HTTP (same as online imagery dialog)
- No QRegExp (use QRegularExpression)
- No deprecated signal/slot syntax

### 6. Good OSM citizenship

- `User-Agent: OpenOrienteering Mapper/{version}` header
- Respect Overpass API rate limits (no more than 1 concurrent request)
- Timeout after 30 seconds
- Bounding box area limit with user warning (same pattern as imagery)
- Query only tags we actually use (not `[*]` wildcard)
- Cache nothing server-side — Overpass handles its own caching

## New Files

```
src/gdal/overpass_client.h        # Overpass API query builder + fetcher
src/gdal/overpass_client.cpp      # ~150 lines: build query, send, save
src/gui/widgets/osm_import_dialog.h    # Simple fetch-and-import UI
src/gui/widgets/osm_import_dialog.cpp  # ~200 lines: extent, options, progress
```

## Modified Files

```
src/gui/map/map_editor.h/cpp     # Menu entry + slot
src/CMakeLists.txt                # New source files
src/gdal/CMakeLists.txt           # New source files (if in gdal/)
```

## Files NOT Modified

- `ogr_file_format.cpp` — we call into it, don't change it
- `symbol_rule_set.cpp` — we use it as-is
- `mapper-osmconf.ini` — already has the right tags
- CRT files — improvements in separate commits/PRs

## Overpass Query Construction

Given CRT rules referencing tags like `natural`, `highway`, `building`,
`waterway`, `landuse`, `leisure`, `barrier`, `man_made`, `power`,
`railway`, `amenity`, `tourism`, `historic`:

```
[out:xml][timeout:30][bbox:{south},{west},{north},{east}];
(
  nwr["natural"];
  nwr["highway"];
  nwr["building"];
  nwr["waterway"];
  nwr["landuse"];
  nwr["leisure"];
  nwr["barrier"];
  nwr["man_made"];
  nwr["power"];
  nwr["railway"];
  nwr["amenity"];
  nwr["tourism"];
  nwr["historic"];
);
out body;
>;
out skel qt;
```

This fetches all nodes/ways/relations with any of the orienteering-
relevant tags, then resolves referenced nodes for geometry.

### Area limits

Overpass API has server-side limits. For our purposes:
- Warn above 5 km^2 (significant data volume for dense urban areas)
- Hard limit at 50 km^2 (Overpass will likely timeout anyway)
- These match roughly with orienteering map coverage areas

## UI Design

Minimal dialog, consistent with OnlineTemplateDialog:

```
┌─────────────────────────────────────────┐
│  Import OpenStreetMap Data              │
│                                         │
│  Coverage: map extent (~2.3 km^2)       │
│                                         │
│  Symbol set: [ISOM 2017-2        ▼]    │
│                                         │
│  ☐ Open symbol review after import      │
│                                         │
│            [Fetch & Import] [Cancel]    │
│                                         │
│  Fetching OSM data...                   │
└─────────────────────────────────────────┘
```

After import, if "Open symbol review" is checked, the existing
SymbolReplacementDialog opens showing the CRT assignments. This
reuses existing UI with zero new code.

## CRT File Audit (Separate Work)

The iofSymbolSets repo has machine-readable ISOM/ISSprOM symbol lists
(TSV format, 114 ISOM symbols, 106 ISSprOM symbols). We can audit the
shipped CRT files against these authoritative lists to find:

1. Symbols with no OSM mapping (coverage gaps)
2. Symbol numbers that don't match current IOF standards
3. Tag patterns that could be improved

This audit should produce:
- Updated CRT files as standalone commits (easy upstream acceptance)
- A coverage report showing what % of IOF symbols have OSM mappings

This is a **data contribution** separate from the code feature, making
it easy to review and accept independently.

## Git Strategy

### Branch: `osm-vector-import` from `master`

Independent of `tiled-raster-loading`. No shared modified files.

### Commit sequence

1. **CRT file updates** (if audit finds improvements) — data-only,
   trivially reviewable, high standalone value
2. **OverpassClient class** — self-contained network client with tests
3. **OsmImportDialog** — UI that wires Overpass fetch to OGR import
4. **Menu integration** — Templates menu entry + MapEditorController slot
5. **Tests** — mock Overpass response -> verify import + symbol assignment

### PR strategy

The CRT updates could be a separate tiny PR for fast acceptance.
The import feature is a single focused PR with clear scope.

## What This Does NOT Do

- Replace the existing File > Import workflow (still works for .osm/.pbf)
- Create new symbol types or definitions
- Modify the OGR import pipeline
- Add OSM editing capabilities
- Cache OSM data between sessions
- Handle conflation (merging OSM with existing hand-drawn objects)

## Future Possibilities (out of scope)

- Selective re-fetch for changed areas
- Incremental updates via Overpass diff API
- Custom tag-to-symbol rules editor
- Multi-standard import (fetch once, symbolize for both ISOM and ISSprOM)
- OSM data as a background reference layer (read-only, not imported)
