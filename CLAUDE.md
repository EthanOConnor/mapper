See `AGENTS.md` for the full local guidance.

# Full-Speed-Ahead Worktree

Experimental branch for pushing all dev goals forward fast — tiled raster
loading, online imagery, render improvements, and more. Not upstream-bound.

- Combine features, discover integration issues, record learnings.
- Clean replay into `pr/*` branches happens later.
- Respect project coding style and Qt6 conventions, but don't slow down for
  upstream polish, commit hygiene, or PR splitting.
- Move fast, keep the build healthy, and note what you learn.
- See `/Users/ethan/dev/oom/` for the full workspace layout and branch strategy.

## Remotes

- `upstream` — OpenOrienteering/mapper (the canonical repo; PRs target here)
- `origin` — EthanOConnor/mapper (our fork; feature branches push here)

## Build

```sh
mkdir -p build && cd build
cmake ..
make -j$(sysctl -n hw.logicalcpu)
```

## Test

```sh
cd build && ctest --output-on-failure
```
