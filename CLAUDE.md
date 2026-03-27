See AGENTS.md for engineering principles and contribution discipline.

# OpenOrienteering Mapper — Contributor Workspace

We are external contributors to [OpenOrienteering Mapper](https://github.com/OpenOrienteering/mapper), an open-source orienteering mapmaking application (C++/Qt/CMake, GPL v3). Our goal is to submit pull requests that have a strong chance of upstream acceptance.

## Remotes

- `upstream` — OpenOrienteering/mapper (the canonical repo; PRs target here)
- `origin` — EthanOConnor/mapper (our fork; feature branches push here)

Feature branches always start from `upstream/master`. CLAUDE.md and AGENTS.md live only on our fork's master and must never appear in PR branches.

## Build

```sh
mkdir -p build && cd build
cmake ..
make -j$(sysctl -n hw.logicalcpu)
```

See `INSTALL.md` for platform-specific prerequisites (Qt >= 5.5, PROJ, GDAL, Clipper, ZLib). On macOS, the OpenOrienteering superbuild project is the recommended way to handle dependencies.

## Test

```sh
cd build && ctest --output-on-failure
```

## Code quality

```sh
./codespell.sh        # spell check
```

The project enforces clang-tidy (config in `.clang-tidy`) and defines coding style in `doc/coding-style.xml`.
