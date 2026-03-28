# Engineering Principles and Contribution Discipline

## Document authority

| Document | Role |
|---|---|
| Upstream `README.md` | Product truth (theirs, not ours to modify) |
| Upstream `INSTALL.md` | Build instructions (theirs) |
| `AGENTS.md` | Our engineering invariants and contribution discipline |
| `CLAUDE.md` | Quick-reference entry point for AI agents |
| `DESIGN-tiled-raster.md` | Active design for viewport-aware GDAL raster loading |

Everything else in the repo is upstream's. Read it, learn from it, contribute to it — but always through PRs.

## Mission

Submit pull requests to OpenOrienteering/mapper that:
1. Fix real bugs or add real value
2. Follow upstream conventions exactly
3. Include appropriate tests
4. Are small, focused, and easy to review
5. Have commit messages that match the project's existing style

We are guests in this codebase. Upstream acceptance is the measure of success.

## Cross-project engineering principles

These principles apply across all our work, regardless of language or project.

### Concise, not clever
Write the simplest code that solves the problem. No abstractions for hypothetical future needs. Three similar lines beat a premature helper function.

### Deep-patterned, not special-cased
Prefer systematic solutions over one-off fixes. If a problem appears in one place, understand whether it's an instance of a broader pattern and solve the pattern. A single well-chosen abstraction applied consistently beats ten scattered `if` statements. The code should read as an expression of the underlying structure, not a patchwork of exceptions.

### Match the texture of the codebase
New code should be indistinguishable from existing code in style, idiom, and level of abstraction. Read surrounding code before writing. If the project uses Qt's endian API, use Qt's endian API — don't invent a wrapper. If the project doesn't use `auto`, neither do we. The goal is a PR that looks like it was written by someone who's been on the project for years.

### No overengineering
Don't add features, configurability, or "improvements" beyond what was asked. A bug fix doesn't need surrounding code cleaned up. Stay in scope.

### Functional where possible
Prefer pure functions over stateful methods. Keep side effects at boundaries. Predictable code is reviewable code — doubly important when contributing to someone else's project.

### Data in files, not code
Configuration, constants, and data belong in data files or resource systems, not hardcoded in source.

### No god classes
If a class does everything, it's doing too much. Respect the existing separation of concerns in the codebase.

### Documentation tracks reality
If you change behavior, update the relevant docs in the same change. Don't add documentation where none exists upstream — that's a separate conversation with maintainers.

### Low resource consumption
Treat CPU, memory, and battery as finite. Don't introduce expensive operations without justification.

## Upstream conventions — non-negotiable

These are the project's own conventions. We follow them exactly.

### Coding style
- Tabs for indentation, 4-space tab width
- Pointer/reference binds to type: `Type* var`, not `Type *var`
- Formatting per `doc/coding-style.xml`
- clang-tidy clean against `.clang-tidy` before submitting
- Run `./codespell.sh` before submitting

### Commit messages
Match the existing style. Pattern: `Component: Imperative description` or `area: description`.
```
SensorsTest: Allow skipping the Powershell source test
TemplateTest: Fix Unicode string initialization
test: Capture output from WIN32 executable
packaging: More time for hdiutil
Fix GDAL use in CMake (#2480)
```
Reference issue numbers with `(#NNN)` where applicable.

### C++ standard
C++14 is the project baseline. Don't use later features unless the project has already adopted them in the area you're touching.

### Qt6 forward compatibility
Qt6 migration is on the horizon (#2483). All new code must use only Qt6-safe APIs:
- No `QStringRef` (use `QString` or `QStringView`)
- No `QMatrix` (use `QTransform`)
- No `QRegExp` (use `QRegularExpression`)
- No `QPrinter::PaperSize` (use `QPageSize`)
- Match existing project conventions for types (`int` not `qsizetype`, etc.)

### Build system
CMake. Don't introduce new build tools or dependencies without strong justification and upstream discussion first.

### Testing
Qt Test framework, run via CTest. Changes to core functionality must include test coverage. Test files follow the pattern `ComponentTest.cpp` or `component_test.cpp`.

## PR discipline

### Before starting work
1. Check the [issue tracker](https://github.com/OpenOrienteering/mapper/issues) for existing discussion and context
2. Read the relevant existing code thoroughly before proposing changes
3. Understand why the code is the way it is — there may be non-obvious reasons
4. Branch from `upstream/master`: `git checkout -b fix-topic upstream/master`

### Crafting a PR
- One logical change per PR — don't bundle unrelated fixes
- Small PRs review faster and merge easier
- If a change requires multiple steps, consider a series of PRs
- Run all existing tests and code checks before pushing
- Clear PR description: what changed, why, and how to verify
- Push to `origin`, PR targets `upstream/master`

### Dependency updates
Dependency changes go in their own dedicated PRs. Never bundle a dependency update with a feature or bugfix.

### What NOT to do
- Don't reformat code outside your diff (style-only changes are separate PRs)
- Don't add dependencies without upstream discussion
- Don't change build infrastructure speculatively
- Don't submit half-finished work hoping for feedback — submit when it's ready
- Don't include `CLAUDE.md`, `AGENTS.md`, or AI-tooling files in PR branches
- Don't introduce C++ features beyond the project's established standard
- Don't add comments, docstrings, or type annotations to code you didn't change
