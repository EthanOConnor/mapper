# Modernization foundation closeout plan

Status: authoritative remaining-work contract for the modernization foundation
after `modernization-checkpoint-16-input-parity`.

This plan incorporates the implementation retrospective and its independent
verification. It supersedes any older closeout sequence or acceptance wording
that conflicts with it. The broader campaign design remains in
[`README.md`](README.md); this document says how to finish it.

## Outcome

Reach one public, tagged `main` revision that is honestly describable as
modernized, reflected upon, simplified, and ready for feature development.
Until the final tag exists, the branch remains in a feature freeze.

This is still pure modernization. Do not add GNSS, course design, imagery,
Purple Pen, georeferencing, or other product features. Preserve existing
product scenarios while replacing obsolete technology and deleting accidental
or speculative machinery.

The governing test for every change is:

> Does this make the current product easier to build, understand, test,
> operate, or evolve while preserving real Mapper behavior?

New machinery is welcome only when it makes the whole system smaller or gets a
real gate to completion materially faster. A new tool, abstraction, workflow,
or test harness must replace more complexity than it adds. Do not build a
framework to administer this rule.

## Definition of done

The foundation is done only when all of these are true at the same commit:

- A single external frame request converges a heavy raster view without an
  unrelated event, a busy loop, or a hidden second request.
- Dead, test-only, misleading, and abandoned renderer contracts have been
  removed or reduced to an explicit, justified production decision.
- Renderer, frame-planning, domain-snapshot, and native-presentation ownership
  boundaries are visible in the types and build targets rather than prose
  alone.
- All supported targets build and test on the latest sensible dependency and
  runner baseline, with any deliberate pin explained.
- GitHub Actions is green at the exact commit, stays conventional and readable,
  and has measured build and cache behavior with sustainable cache use.
- The modernization and acceptance documents describe the code that actually
  exists, the current test matrix, and current evidence.
- Final evidence is built from the canonical checkout, and the documented CMake,
  Corrosion, and direct Cargo paths put generated output under `build/` rather
  than the source tree.
- The live three-map comparison against `full-speed-ahead` passes for visual
  quality, interaction, and fluidity, with a human verdict recorded.
- Validation-only product instrumentation has been removed or extracted after
  it has served the live gate.
- The exact public commit is annotated, tagged, and pushed as
  `modernization-foundation-final`.

Green unit tests alone do not satisfy this definition.

## Source and workspace rules

In the OOM workspace, work from `worktrees/main`, the canonical public `main`
checkout described by the workspace-root `AGENTS.md`. The adjacent
`worktrees/modern-core` path is only a compatibility symlink for old notes and
build records; do not use it in new commands, evidence, or documentation.
Outside that workspace, use an ordinary clean checkout of public `main`.

Confirm `main`, a clean worktree, and the public `EthanOConnor/mapper` `origin`
before changing code. Use the detached `repo/` checkout only for graph, ref,
remote, and worktree administration. Repository-relative paths in this plan are
portable; workspace paths name roles, not source dependencies.

Treat `full-speed-ahead` and the private historical branches as behavioral and
implementation oracles, never ancestry to merge wholesale. Preserve the exact
upstream mirror on `master`; upstream candidates continue to start there.

At the beginning of each phase, re-read the applicable `AGENTS.md` and
`CLAUDE.md`, fetch current refs, inspect the live tree, and replace stale facts
in this plan with verified evidence in the acceptance record.

## Checkpoint discipline

Use coherent commits inside each phase. Create the following annotated tags only
after the tagged revision is clean locally and the exact revision has a green
hosted matrix:

1. `modernization-checkpoint-17-raster-convergence`
2. `modernization-checkpoint-18-contract-cleanup`
3. `modernization-checkpoint-19-foundation-candidate`
4. `modernization-foundation-final`

Push each checkpoint commit and tag. Record the commit, GitHub Actions run,
commands, artifacts, and remaining work in
[`test/manual/rewrite-foundation-acceptance.md`](../../test/manual/rewrite-foundation-acceptance.md).
Do not create tags for documentation-only microsteps or label a commit whose
hosted run belongs to another SHA.

## Phase 0: refresh the baseline

Before editing:

1. Confirm the public `main` worktree, remotes, current HEAD, clean status, and
   divergence from `origin/main`.
2. Run the release configure/build and enumerate, rather than assume, the
   current CTest total. Run the renderer, input, packaging, and dependency tests
   relevant to the files below.
3. Inspect the newest hosted run and artifacts for the exact HEAD.
4. Reconfirm every issue named in this plan against production callers. Delete
   or amend any item already resolved by concurrent work.
5. Capture current cold/warm CI durations, cache hits, and cache sizes before
   changing the workflow.
6. Inspect retained `CMakeCache.txt` and Rust dependency files for the old
   `worktrees/modern-core` source path. They were intentionally preserved across
   the directory move, but they are migration scaffolding. Recreate the builds
   used for final evidence from `worktrees/main`; a successful build through
   the compatibility symlink is not final-path verification.

The 2026-07-15 snapshot was public HEAD
`9e607b98939552a7c05141cdf54256242e44faa5`, 36 discovered CTest tests, and green
hosted run [29435612841](https://github.com/EthanOConnor/mapper/actions/runs/29435612841).
It is a starting observation, not a fact to copy forward without checking.

## Phase 1: make raster frames converge

This is the blocking correctness issue and comes first.

The current planner limits newly resident raster images per frame. A heavy view
with more ready images can return an incomplete packet, but
`FramePacket::raster_complete` is not consumed and nothing schedules the next
product frame. The view can stall until unrelated invalidation.

Implement the smallest explicit convergence contract:

- When a frame is incomplete and made residency progress, schedule one further
  paced product frame.
- When it is incomplete but made no progress because source data is not ready,
  wait for the existing source-ready notification. Do not poll or spin.
- Coalesce redundant requests and preserve interactive responsiveness.
- Keep the decision in the coordinator that knows both frame outcome and
  scheduling, not as an ignored packet flag.
- Remove `FramePacket::raster_complete` and corresponding request plumbing once
  the real coordinator contract exists.

Add a product-level regression test proving that one external render request
converges a view needing more than one residency batch. Add focused coverage for
the no-progress/missing-source case to prove it does not busy-loop. The test
must fail if convergence again depends on a second external event.

Run the full local suite and hosted matrix, update the acceptance record, then
tag `modernization-checkpoint-17-raster-convergence`.

## Phase 2: remove false contracts and finish the boundaries

Do this as a deletion-led audit. Reconfirm callers, delete the dead shape, then
repair the narrowest resulting interface.

### Delete confirmed dead paths

- Delete `FrameRequestHandler`, its setter/member, `requestFrame()`, the
  `QEvent::UpdateRequest` handler, and the now-pointless event-filter
  installation if live inspection still shows no producer and no reachable
  update path.
- Remove render-command `object_id` if production still never reads it. Keep
  stable object identity in the domain snapshot where it has a real purpose.
- Remove `drawOverprintingSimulation()` if it remains uncalled.
- Remove the unused backend result field and unused `QTouchEvent` include.

### Make the renderer boundary real

- Split snapshot-free frame data from planner/domain ownership. A backend frame
  packet must not carry a domain snapshot merely because the planner did.
- Split native surface state/descriptor data from the Qt object that owns the
  `QWindow`. Backends may consume the narrow surface description they need;
  they must not depend on presentation ownership.
- Enforce these directions with ordinary target boundaries and focused tests.
  Do not invent a renderer plugin framework or dependency-rule language.

### Make explicit production decisions

- If glyph-run machinery remains test-only while production text is path based,
  remove the unused machinery and state the path-text decision plainly. The old
  glyph note documented an exploratory lesson, not a completed production
  claim; do not port a bespoke font registry merely to match it.
- Remove the text-antialiasing preference if it still affects only QPainter
  while the supported screen backend ignores it. Otherwise give it one tested,
  backend-consistent meaning. Prefer a deterministic reference/export policy
  over a misleading user setting.
- Remove UIKit/iOS code and conditionals if iOS is still outside `SUPPORT.md`.
  If support has been intentionally added, it instead needs a first-class
  preset, hosted gate, and support contract; do not preserve residue as a hedge.

Preserve mechanisms with demonstrated product evidence, including Windows WARP
runtime support, Linux Xvfb, Qt 6 icon-loader handling, the GDAL overlay,
QPainter output/reference rendering, and native-surface input forwarding. Do
not simplify the cursor workaround or transparent-for-mouse-events handling
without cross-platform proof that the code is redundant.

Run the full local suite and hosted matrix, update the acceptance record, then
tag `modernization-checkpoint-18-contract-cleanup`.

## Phase 3: make CI fast, sustainable, and unremarkable

Retain the good foundation: standard GitHub-hosted runners, job concurrency,
least-privilege permissions, immutable full-SHA action pins with readable
release comments, Dependabot updates, native target tests, release provenance,
and uncompressed transfer of already-compressed artifacts.

The 2026-07-15 baseline was approximately 8 minutes on macOS, 10 minutes on
Linux and Android, and 27 minutes on Windows. Cache usage reported by GitHub was
10.40 GB across 21 entries: about 2.24 GB Qt, 4.67 GB Rust, and 3.50 GB vcpkg.
That is effectively at the default repository cache budget and risks eviction
churn. Re-measure rather than assuming those numbers remain current.

Use the official GitHub guidance for
[dependency caching](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching),
[secure action pinning](https://docs.github.com/en/actions/reference/security/secure-use),
[least-privilege tokens](https://docs.github.com/en/actions/tutorials/authenticate-with-github_token),
and [concurrency](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency).

The CI change is complete only when measurements support it:

1. Record cold and warm timings, hit/miss state, and bytes for each retained
   cache before and after the change.
2. Let untrusted pull requests restore safe dependency caches but not publish
   durable cache state. Save new caches from trusted `main`/tag runs only, after
   successful build and tests. Use explicit restore/save consistently where
   that policy matters.
3. Include every compatibility input in the key: cache schema, runner/OS,
   architecture, compiler/toolchain, dependency-manager version, profile or
   triplet, lockfiles/manifests, overlay ports, and relevant tool versions.
   Separate immutable dependency downloads from mutable compiler outputs unless
   a measured combined cache is simpler and demonstrably better.
4. Keep active usage below 8 GB as an operating target, leaving eviction
   headroom. Remove obsolete cache families only after their replacements have
   populated and passed. Never cache secrets or final release artifacts.
5. Delete caches whose restore/upload cost is not repaid. Prefer official setup
   actions' built-in caching when it exactly matches the required key and trust
   policy; otherwise keep a small explicit `actions/cache` flow.
6. Attack the measured Windows critical path first: the build, Rust tooling,
   dependency restore, and documentation setup. Do not introduce a custom runner
   image, self-hosted runner, or bespoke package service without evidence that
   it reduces total maintenance and wall time.
7. Keep the workflow in one readable file unless extracting a reusable component
   measurably removes duplication. Add a pinned standard checker such as
   `actionlint` only if it replaces ad hoc validation and stays low maintenance.
8. Keep CMake, Corrosion, and direct Cargo output under the active
   `build/<preset>/` tree. Give direct Rust checks one documented build-owned
   target directory, remove `src/render/vello/target`, and do not share mutable
   target output across incompatible host, Android, or configuration builds.
9. Validate YAML, presets, packages, installation/startup smoke checks, release
   provenance, and the exact hosted matrix after restructuring.

The brief attempt to use Ninja 1.13.2 failed because that version was not
available from the chosen simple PyPI install channel; 1.13.0 was restored.
Document that rationale. Re-evaluate current releases and official channels,
but do not add a custom download path merely to claim a higher version.

## Phase 4: reconcile documentation and create the candidate

Make the docs an accurate map of the code rather than an aspirational
specification:

- Update the acceptance report to the exact candidate commit, hosted run, test
  discovery count, artifacts, and unresolved human/hardware gates.
- Explain that the seven items in the campaign README are architectural recovery
  layers, while numbered checkpoint tags record multiple verified recovery
  points. Do not imply that only seven tags exist.
- Remove hard-coded current test totals from evergreen prose; keep totals only
  in dated evidence.
- Prune the unused corpus manifest and the unimplemented 50-map/SSIM/four-hour
  soak language from the foundation gate, or move genuinely useful ideas to a
  clearly non-blocking future-evaluation note. Do not create a corpus platform
  to make old prose true.
- Re-audit claims about glyphs, antialiasing, raster completion, backend
  neutrality, platform support, and dependency versions against production
  code.
- Record the Ninja version/channel decision and any other deliberate non-latest
  pin next to the current baseline.

Then perform the live gate documented in
[`test/manual/render-parity.md`](../../test/manual/render-parity.md): use the
same three representative maps and interaction traces on the candidate and
`full-speed-ahead`, and assess graphic quality, fluidity, blanking/seams,
cursors, selection/drawing, pan/zoom/rotation, and overlays. Record the human
verdict and any intentional difference. Automated image metrics can assist but
cannot replace this gate.

Keep the existing `MapWidget` parity driver only through this comparison. Once
the exact candidate is green and the live result is recorded, tag
`modernization-checkpoint-19-foundation-candidate`.

## Phase 5: retire validation residue and finalize

After the live gate—not before—remove the product-embedded parity driver or
extract the smallest durable benchmark/test that uses normal product APIs.
Retain timing instrumentation only if it has an active consumer and reduces
future diagnosis cost; otherwise delete it. The shipped product must not carry
a validation control plane for a completed migration.

Run the release configure/build, all discovered tests, focused renderer and
input tests, packaging/startup smoke checks, benchmarks, and the full hosted
matrix one final time. Update the acceptance report to the exact SHA and ensure
the tree and documentation contain no remaining foundation TODO presented as
complete.

Create and push the annotated tag `modernization-foundation-final`. Feature
development may resume only from that public tag or a direct descendant.

Physical Windows print verification and native Android document-URI/device
verification remain release-candidate gates if they cannot be performed on
hosted infrastructure. They must be stated plainly with prepared binaries and
instructions; they do not justify pretending the foundation is untested, nor
do they justify inventing simulated platform machinery.

## Phase 6: close the local campaign workspace

After the public final tag exists, perform only the bounded local cleanup that
the finished workspace topology assigns to this campaign:

- Verify `worktrees/full-speed-ahead` is clean and its HEAD is preserved by the
  private `internal/full-speed-ahead` ref, then remove that temporary worktree
  through the detached `repo/` administration checkout.
- Distill any still-useful findings from `dev/modernize/` into the public docs,
  then mark or archive that research area as completed and update
  `dev/README.md`. Do not reorganize unrelated research topics.
- Remove superseded local build directories after final evidence has been
  retained under `artifacts/`, and confirm Phase 3 left no generated Rust output
  in the source tree.
- After current build caches have been recreated from `worktrees/main`, verify
  that no active configuration or command depends on `worktrees/modern-core`
  and remove the compatibility symlink. Historical text under `artifacts/` may
  retain the old path as evidence and does not block removal.
- Update the workspace-root `README.md`, `AGENTS.md`, and `dev/README.md` after
  removing the oracle worktree or compatibility symlink so the routing table
  describes the state agents will actually encounter.

Do not touch the deliberately dirty `worktrees/coc-minimal`, promote unrelated
standalone projects, or turn local/public branch hygiene into modernization
work. Those are separate workspace decisions. Phase 6 changes no public source
tag and must not become another product checkpoint.

## Completion behavior

Persist through all phases. Do not stop after a code fix, documentation cleanup,
or a merely green local run. If a genuine human or physical-device gate is the
only remaining blocker, prepare the exact artifact and minimal test script,
record everything already proven, and report that single blocker. Ask for a
scope decision only when proceeding would require feature work, a support-matrix
change, destructive history rewriting, or a materially more complex operational
system.

## Required reading

- [`README.md`](README.md) — campaign goals, scenarios, and architecture
- [`../../AGENTS.md`](../../AGENTS.md) and, in the OOM workspace, its root
  `AGENTS.md` — active repository and worktree policy
- [`../../SUPPORT.md`](../../SUPPORT.md) — supported platforms and toolchains
- [`../../test/manual/rewrite-foundation-acceptance.md`](../../test/manual/rewrite-foundation-acceptance.md)
- [`../../test/manual/render-parity.md`](../../test/manual/render-parity.md)
- [`../../test/manual/windows-print-acceptance.md`](../../test/manual/windows-print-acceptance.md)
- [`../../.github/workflows/ci.yml`](../../.github/workflows/ci.yml)
