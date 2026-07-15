# Agent prompt: finish the modernization foundation

Work on the canonical public `main` checkout for `EthanOConnor/mapper`. In the
OOM workspace this is `worktrees/main`; read the workspace-root `AGENTS.md` and
do not work through the `worktrees/modern-core` compatibility symlink. Outside
that workspace, use an ordinary clean checkout of public `main`. Before editing,
verify the branch, public `origin`, clean status, and every applicable
`AGENTS.md` and `CLAUDE.md`.

Your sole goal is to execute
[`doc/modernization/foundation-closeout-plan.md`](foundation-closeout-plan.md)
end to end and leave a public, exact, hosted-green
`modernization-foundation-final` tag. The plan is the authority for remaining
work. Also read the campaign
[`README.md`](README.md),
[`test/manual/rewrite-foundation-acceptance.md`](../../test/manual/rewrite-foundation-acceptance.md),
[`test/manual/render-parity.md`](../../test/manual/render-parity.md), and
[`SUPPORT.md`](../../SUPPORT.md).

This is pure modernization, not feature development. Re-derive the best final
shape from current product scenarios; do not preserve exploratory structure for
historical reasons. Use `full-speed-ahead` as a visual and behavioral oracle,
never as ancestry to merge wholesale. Prefer deletion and ordinary library,
language, Qt, CMake, and GitHub mechanisms. Introduce machinery only when it
removes more complexity than it adds or measurably shortens the path to a real
gate. Do not build a modernization governance framework.

Follow the plan in order. First fix and prove multi-batch raster convergence.
Then delete dead renderer contracts and finish the backend/presentation
boundaries. Then simplify and measure GitHub Actions: preserve full-SHA action
pins, least privilege, concurrency, hosted-native tests, provenance, and
Dependabot; redesign cache keys and trust-aware writes from measured cold/warm
timings and byte use, keep cache occupancy sustainable, and optimize the
Windows critical path without custom infrastructure unless the measurements
compel it. Keep direct Cargo and Corrosion output inside build-owned directories,
not the source tree, and recreate retained CMake caches so final evidence no
longer depends on the `modern-core` compatibility path. Reconcile every
modernization claim with production code and current evidence. Run the
three-map live comparison, record the human verdict, and only then retire
product-embedded parity instrumentation.

Commit coherent changes and create the annotated checkpoint tags named in the
plan only after local verification and a green hosted matrix for the exact SHA.
Push each checkpoint commit and tag, update the acceptance record with exact run
and artifact links, and continue until the final tag exists. Never weaken a
test, gate, or support claim to get green. Do not stop merely because tests pass
or one phase is complete. If the only remaining requirement needs a human or
physical device, prepare the exact binary and minimal instructions and report
that one blocker with everything else completed. After the public final tag,
perform the plan's bounded campaign-workspace retirement; do not touch the dirty
`coc-minimal` worktree or drift into unrelated branch/research cleanup.
