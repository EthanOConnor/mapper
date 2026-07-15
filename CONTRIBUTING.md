# Contributing

This repository is both a proper fork of OpenOrienteering Mapper and the
canonical source for the forward COC product. Branch direction keeps those two
purposes unambiguous.

## Choose the destination first

- `master` is an exact mirror of `OpenOrienteering/mapper:master`. Do not add
  downstream commits to it.
- `pr/<topic>` branches are focused upstream candidates. Create each one from
  `master` and target the upstream repository. Never merge `main` into them.
- `main` is the forward COC product and the default branch. Product fixes,
  dependency updates, CI, packaging, and releases converge here.
- Product topic branches start from `main` and are short-lived.

GitHub blocks deletion and non-fast-forward updates of `main` and `master`.
This is intentionally a small safety rail rather than a mandatory pull-request
workflow: maintainers may push reviewed fast-forward commits directly, while
rewriting either canonical branch remains prohibited.

If a product change is suitable upstream, re-derive the smallest maintained
change from `master`. The product branch is evidence and a behavioral oracle,
not ancestry for the pull request.

## Historical work

The earlier experimental history is preserved privately in
`EthanOConnor/mapper-internal`. It is intentionally not mirrored here. The
public `mapper-coc` repository preserves the COC.5 release/download site; it is
not a second canonical source tree.

## Verification

Changes to `main` must keep the supported desktop matrix and Android package
build green. Rendering changes require reference-output and Vello conformance
coverage. Platform-specific behavior must be tested on the affected platform;
do not convert a failure into a skip merely to close a change.

The modernization architecture, acceptance scenarios, and recorded benchmark
baselines are in [`doc/modernization/README.md`](doc/modernization/README.md).
