# Connected Map Hub workflow

Mapper remains a complete standalone editor. The connected workflow adds a
server-backed lifecycle around an ordinary native `.omap` file; it does not
replace `.omap`, embed a database document in it, or weaken OCAD import/export.

## User flow

1. Open **Map Hub** and enter the HTTPS server. **Connect in browser** creates a
   short-lived device request, opens the same-origin verification page, and
   lets Mapper poll the exchange until the browser approves it. Passkey
   creation is the default and a password remains available. Pasting a
   previously issued Mapper API token remains an explicit fallback.
2. Open **Map Hub — library and my work** from the home screen, File menu, or
   toolbar. The two views show the current library and assignments for the
   connected account. Library records expose the exact approved revision and
   checksum, but never open a detached editable copy; editing starts from an
   assigned managed workspace. Course-design and print-production assignments
   remain visible, but are managed in Map Hub instead of being opened as map
   workspaces; Purple Pen remains the producer of their course files and PDFs.
3. Starting an assignment asks the server for the current workspace. People
   assigned to the same non-exclusive work package join one canonical shared
   workspace; exclusive packages retain their deliberate checkout. Each
   person/device receives its own renewable, bearer-bound editing lease, so a
   second mapper no longer invalidates the first mapper's session. Mapper
   downloads the approved base to a new local workspace, verifies its SHA-256
   before opening it, and
   synchronizes project-authorized raster tile layers into the immutable OIC
   catalog store. A native OMAP base remains OMAP. An OCAD base is preserved
   byte-for-byte beside a normalized native `.omap` editing workspace.
4. Normal Save is always local and offline-capable. Live drawing, symbol, and
   map-part edits are committed to an app-private SQLite operation journal
   before they are reported as locally safe. **Checkpoint to Map Hub**
   saves first, uploads the exact native `.omap`, supplies the exact base
   revision and a stable idempotency key, and records the returned immutable
   revision. A stale base never overwrites local work.
5. **Submit for review** checkpoints if necessary and submits that exact
   revision. Map Hub refuses to force-submit while another unreleased Mapper
   session could still own durable work; `409 collaborators_active` preserves
   the checkpoint and workspace. Teammates finish and use **Leave connected
   editing**, or let their lease expire, before submission releases the leases.
   Approval remains a server-side librarian or director action.
6. **New connected map** captures one or more venues, predecessor lineage,
   work type, assignee, source provenance, target CRS/scale/symbol standard,
   and exclusive-editing policy. It creates the database project, work
   package, assignment, and server workspace first. Only after the idempotent
   server transaction succeeds does Mapper create and save the local `.omap`.
   Mapper locks the required scale, configures and validates the CRS, and
   restricts installed symbol-set choices to the required standard (with an
   explicit confirmation for a custom symbol file) before binding the file.

The editor exposes the same connected-work surface on desktop and mobile. It
separates edits safe on this device from edits acknowledged by Map Hub, shows
pending operations and active collaborators, identifies the immutable revision
and live stream head, and keeps retry, checkpoint, and submission actions
available even when the platform menu/status bar is hidden. A conflict or
expired authorization remains visible across later local edits and relaunches.

## Local records and credentials

Managed-workspace records live under Qt's application data directory in
`managed-workspaces/`, addressed by the canonical local map path. They contain
stable organization/project/work-package/workspace/revision IDs, checksums, and
lease expiry, but no bearer secret. They are app-private sidecars, not siblings
of the map, so exporting or emailing an `.omap` does not disclose lifecycle
state.

The account token and each workspace lease use iOS Keychain, Windows Credential
Manager, or an Android Keystore-backed encrypted value. Linux uses Secret
Service when `secret-tool` is available; minimal Unix systems use an explicit
owner-only application credential file fallback. Locally rebuilt/ad-hoc-signed
macOS Mapper also uses that owner-only app configuration file, avoiding unstable
Keychain ACL prompts across rebuilds. Tokens are never stored in QSettings,
URLs, imagery catalogs, or map documents.

Map Hub can register an in-memory bearer credential for its exact tile origin.
The tiled network scheduler sends it only to that exact origin, strips it on a
cross-origin redirect, and bypasses the shared HTTP disk cache for authenticated
requests. Project manifests containing credential-like tile URL query fields
are rejected instead of being persisted.

## API contract used by Mapper

- `POST /api/v1/auth/mapper/connect`
- `POST /api/v1/auth/mapper/connect/{request_id}/exchange`
- `GET /api/v1/library`
- `GET /api/v1/projects/{project_id}/manifest`
- `POST /api/v1/projects` with `Idempotency-Key`
- `POST /api/v1/assignments/{assignment_id}/start` with a stable
  `X-Editing-Client-Instance`, the legacy Mapper alias, and any retained
  `X-Editing-Lease`
- `GET /api/v1/artifacts/{artifact_id}/download`
- `POST /api/v1/workspaces/{workspace_id}/checkpoint` with
  `Idempotency-Key`, exact `base_revision_id`, and `X-Editing-Lease`
- `POST /api/v1/workspaces/{workspace_id}/renew`
- `POST /api/v1/workspaces/{workspace_id}/release`
- `GET /api/v1/workspaces/{workspace_id}/sync-state` with
  `X-Editing-Client-Instance` (and the legacy
  `X-Mapper-Client-Instance` alias during migration)
- `GET /api/v1/workspaces/{workspace_id}/operations`
- `POST /api/v1/workspaces/{workspace_id}/transactions` with
  `X-Editing-Lease`
- `POST /api/v1/workspaces/{workspace_id}/ack` with `X-Editing-Lease`
- `POST /api/v1/workspaces/{workspace_id}/snapshots` with
  `X-Editing-Lease`
- `POST /api/v1/revisions/{revision_id}/submit` with `X-Editing-Lease`

All protected calls use `Authorization: Bearer …`; Mapper accepts a non-TLS
server only on localhost for development. JSON responses are capped at 16 MiB;
map transfers are capped at 2 GiB and every download write and final checksum
must succeed before Mapper opens the artifact.

The client-instance UUID is durable for an assignment and is persisted before
the first start request, so retrying after a lost response identifies the same
native session. Map Hub binds each lease to workspace, person, and client
instance. A repeated start with that identity and its retained secret renews
the exact lease; a repeated start after the secret is lost atomically releases
only that client's old lease and rotates a replacement. Different client
instances retain independent leases. Mapper stores lease secrets by
`(server origin, workspace, client instance)` and migrates the former
workspace-only secret after verifying it. It sends both client-instance header
names with the same value on assignment start and every lease-bound mutation
(checkpoint, transaction, snapshot, ACK, renew, release, and submit) while the
legacy alias remains supported.

`sync-state` may advertise `sync.poll_after_ms`, `idle_poll_after_ms`, and
`presence_ttl_seconds`. Mapper bounds these hints, polls faster while active,
and treats push/presence only as freshness information: the verified hash-chain
and REST catch-up remain authoritative. Its ephemeral `presence[]` entries
identify a person and client instance, display name, activity, last-seen time,
and applied stream cursor. Repeating ACK at the current applied cursor is the
heartbeat and may include `presence: {state, device_name}`. Routine close sends
`away` and retains the lease for seamless reopen. `/release` is terminal for
that token and is reserved for an explicit **Leave connected editing** action;
Mapper enables Leave only after that device has no pending stream work or
complete-map checkpoint obligation, then detaches the clean file as an
independent local copy. Rejoining goes through assignment start, rejoins the
canonical shared workspace, and receives a fresh lease.

The `/api/v1` route family is the transport generation, while each sync payload
advertises its canonical encoding and materializer independently. Sync
capabilities also advertise the accepted client-instance header names and
client-bound lease semantics. This build requires
`canonical_json: "oom-json/1"` and `protocol: "oom-map-ops/1"` in sync state and
repeats the materializer protocol in transactions, snapshots, and ACKs.
`orienteering-map-ops/1` is reserved for a future neutral semantic model; it is
not an alias for the current payloads. Other clients must negotiate the exact
selection and must not treat OMAP XML fragments as a generic rendering or
storage model.

Transactions intentionally retain strict exact-head acceptance. A
`409 stream_advanced` makes Mapper pull and validate the unseen chain, rebase
locally queued non-conflicting transactions, and retry their exact identities.
Entity/anchor conflicts preserve the outbox and become a durable needs-review
condition; they never turn into a reassuring “saved” label merely because a
later local edit or recovery snapshot succeeds.

## Live-editing completeness boundary

The current `oom-map-ops/1` stream has semantic put/delete operations for
objects, symbols, and map parts. It is not a complete `.omap` document protocol.
The following durable map state is checkpoint-only in this compatibility slice:

- georeferencing, projected CRS, scale, reference frame, declination and
  grivation;
- color definitions and color order;
- template/imagery registrations, source references, transforms and ordering;
- print setup and export metadata;
- map notes and other map-level metadata;
- course data, field media/observations, painted templates, sketches, and other
  external or feature-owned resources.

Most of these edits do not generate an `UndoStep` carrying a stable semantic
inverse. Object tools do; map-part and symbol changes can be reconstructed from
their structural signals. By contrast, georeferencing, notes, print settings,
and related metadata update general dirty state; scale/rotation may clear the
undo stack; colors and templates emit structural signals but lack complete
stable-ID/inverse semantics. Color ordering is also entangled with symbol
references and spot-color composition.

For that reason this build does not add an opaque `map.put` or positional
`color.put/delete`. Applying either could overwrite a collaborator's unrelated
map settings, destabilize color/symbol references, or fail to round-trip OMAP
state. Any detected checkpoint-only mutation instead sets a durable
`checkpoint_required` flag, stages a complete content-addressed recovery
snapshot immediately, and reports **Map settings saved on this device —
checkpoint to share**. Only a successful immutable full-document checkpoint
clears that condition. This is intentionally safer than showing “synced.”

A future protocol revision should add stable document/branch identities and
field-aware operations for georeferencing/map settings, colors, and template
artifact registrations before allowing concurrent writers in those domains.
It should keep drafts, immutable checkpoints, review/approval, and derived
artifacts separate so COC mappers, stewards, field checkers, course designers,
directors, librarians, and printers can collaborate without conflating live
editing with publication.

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
