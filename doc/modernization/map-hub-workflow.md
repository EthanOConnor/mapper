# Connected Map Hub workflow

Mapper remains a complete standalone editor. The connected workflow adds a
server-backed lifecycle around an ordinary native `.omap` file; it does not
replace `.omap`, embed a database document in it, or weaken OCAD import/export.

## User flow

1. Open **Map Hub** and enter the HTTPS server. An invitation opens account
   setup in the system browser, where passkey creation is the default and a
   password remains available. Copy the one-time Mapper connection token back
   into Mapper; it is stored in the operating-system credential store. A
   previously issued Mapper API token can be pasted directly instead.
2. Open **Map Hub — library and my work** from the home screen, File menu, or
   toolbar. The two views show the current library and assignments for the
   connected account. Library records expose the exact approved revision and
   checksum, but never open a detached editable copy; editing starts from an
   assigned managed workspace. Course-design and print-production assignments
   remain visible, but are managed in Map Hub instead of being opened as map
   workspaces; Purple Pen remains the producer of their course files and PDFs.
3. Starting an assignment asks the server for the current workspace and, when
   required, an exclusive editing lease. Mapper downloads the approved base to
   a new local workspace, verifies its SHA-256 before opening it, and
   synchronizes project-authorized raster tile layers into the immutable OIC
   catalog store. A native OMAP base remains OMAP. An OCAD base is preserved
   byte-for-byte beside a normalized native `.omap` editing workspace.
4. Normal Save is always local and offline-capable. **Checkpoint to Map Hub**
   saves first, uploads the exact native `.omap`, supplies the exact base
   revision and a stable idempotency key, and records the returned immutable
   revision. A stale base never overwrites local work.
5. **Submit for review** checkpoints if necessary, submits that exact revision,
   and releases the editing lease. Approval remains a server-side librarian or
   director action.
6. **New connected map** captures one or more venues, predecessor lineage,
   work type, assignee, source provenance, target CRS/scale/symbol standard,
   and exclusive-editing policy. It creates the database project, work
   package, assignment, and server workspace first. Only after the idempotent
   server transaction succeeds does Mapper create and save the local `.omap`.
   Mapper locks the required scale, configures and validates the CRS, and
   restricts installed symbol-set choices to the required standard (with an
   explicit confirmation for a custom symbol file) before binding the file.

## Local records and credentials

Managed-workspace records live under Qt's application data directory in
`managed-workspaces/`, addressed by the canonical local map path. They contain
stable organization/project/work-package/workspace/revision IDs, checksums, and
lease expiry, but no bearer secret. They are app-private sidecars, not siblings
of the map, so exporting or emailing an `.omap` does not disclose lifecycle
state.

The account token and each workspace lease use macOS Keychain, Windows
Credential Manager, or an Android Keystore-backed encrypted value. Linux uses
Secret Service when `secret-tool` is available; minimal Unix systems use an
explicit owner-only application credential file fallback. Tokens are never
stored in QSettings, URLs, imagery catalogs, or map documents.

Map Hub can register an in-memory bearer credential for its exact tile origin.
The tiled network scheduler sends it only to that exact origin, strips it on a
cross-origin redirect, and bypasses the shared HTTP disk cache for authenticated
requests. Project manifests containing credential-like tile URL query fields
are rejected instead of being persisted.

## API contract used by Mapper

- `GET /api/v1/library`
- `GET /api/v1/projects/{project_id}/manifest`
- `POST /api/v1/projects` with `Idempotency-Key`
- `POST /api/v1/assignments/{assignment_id}/start`
- `GET /api/v1/artifacts/{artifact_id}/download`
- `POST /api/v1/workspaces/{workspace_id}/checkpoint` with
  `Idempotency-Key`, exact `base_revision_id`, and `X-Editing-Lease` when
  exclusive
- `POST /api/v1/workspaces/{workspace_id}/renew`
- `POST /api/v1/revisions/{revision_id}/submit` with `X-Editing-Lease` when
  exclusive
- `POST /api/v1/invites/redeem`

All protected calls use `Authorization: Bearer …`; Mapper accepts a non-TLS
server only on localhost for development. JSON responses are capped at 8 MiB;
map transfers are capped at 2 GiB and every download write and final checksum
must succeed before Mapper opens the artifact.

## Public event-map render

`mapper-map-render` is the headless companion for freezing an approved event
revision into the feedback workflow:

```sh
mapper-map-render event.omap event-map.png event-map.json
```

It accepts native OMAP and imported OCAD maps with usable georeferencing. The
saved print area is the output boundary. The PNG contains map objects only—no
authoring helper symbols, templates, or street basemap. The JSON manifest
records the exact source and image SHA-256 values, pixel dimensions, render
resolution, boundary kind, and the four WGS84 image corners in top-left,
top-right, bottom-right, bottom-left order. Map Hub verifies that manifest
against the frozen revision before it exposes the image through an
event-specific feedback capability URL.

The default output's longest edge is 4096 pixels. Use `--max-dimension` for a
different bounded size or `--pixels-per-mm` for a fixed render resolution; no
dimension may exceed 16384 pixels. The tool selects Qt's offscreen platform
automatically when no platform was configured.
