# Local Worktree Guidance

This file is local workspace guidance for the `full-speed-ahead` worktree. It is
not upstream documentation.

## Branch Purpose

This is an experimental integration branch. The goal is to push as many of our
development goals forward as possible — tiled raster loading, online imagery,
render pipeline improvements, and anything else that's ready — recording
learnings and planning for later, more careful upstreamable implementations.

This branch is **not** intended for upstream submission. It exists to:
- Combine and exercise multiple features together in a real build.
- Discover integration issues, performance problems, and design gaps early.
- Record what works, what doesn't, and what the clean upstream version should
  look like.
- Move fast and learn, without the constraints of PR-ready discipline.

Upstream-quality replay of individual features happens later in `pr/*` branches.
See the workspace-level docs in `/Users/ethan/dev/oom/` (`AGENTS.md` and
`CLAUDE.md`) for the full workspace layout, branch strategy, and feature
lifecycle.

## Mission

- Move fast and learn. Ship working combinations of features.
- Record design insights and integration lessons as you go (commit messages,
  code comments, or `dev/` notes).
- Keep the build healthy — broken builds waste more time than they save.
- Don't worry about upstream polish, PR splitting, or commit hygiene here.

## Core Values

- Prefer ultra-clean, straightforward, easy-to-reason-about code.
- Simplify relentlessly. Remove moving parts before adding new ones.
- Use the best well-established practices and knowledge available for the
  actual problem in front of us.
- Make future change easier:
  - reduce coupling
  - keep responsibilities clear
  - create seams that help later refactors and PR splitting
- Be mindful of likely future migrations, especially Qt 6. Avoid pushing new
  code deeper into avoidable Qt 5-specific corners when a neutral design is
  available.
- Separate data, configuration, constants, and naming from logic. Values that
  may need to change later should live in one clear place when practical.
- Write strong, robust code, but do not become paranoid about every
  hypothetical failure mode up front.

## Cross-project Engineering Principles

These principles apply across all our work, regardless of language or project.

### Concise, not clever
Write the simplest code that solves the problem. No abstractions for
hypothetical future needs. Three similar lines beat a premature helper function.

### Deep-patterned, not special-cased
Prefer systematic solutions over one-off fixes. If a problem appears in one
place, understand whether it's an instance of a broader pattern and solve the
pattern. A single well-chosen abstraction applied consistently beats ten
scattered `if` statements.

### Match the texture of the codebase
New code should be indistinguishable from existing code in style, idiom, and
level of abstraction. Read surrounding code before writing.

### No overengineering
Don't add features, configurability, or "improvements" beyond what was asked.
Stay in scope.

### Functional where possible
Prefer pure functions over stateful methods. Keep side effects at boundaries.

### Data in files, not code
Configuration, constants, and data belong in data files or resource systems, not
hardcoded in source.

### No god classes
If a class does everything, it's doing too much. Respect the existing separation
of concerns in the codebase.

### Low resource consumption
Treat CPU, memory, and battery as finite. Don't introduce expensive operations
without justification.

## Design Guidance

- Favor explicit control flow and state flow over cleverness.
- Prefer small helpers or focused objects with clear ownership over giant
  coordinator methods.
- Keep policy separate from mechanism when it materially clarifies the design.
- Keep "what we want", "what we need now", and "how to get it" distinct when
  building async, cached, or progressive systems.
- Avoid duplicated conditional logic, scattered magic values, and hidden
  coupling through shared mutable state.
- Choose the simplest design that solves the real problem well.
- Do not introduce abstraction layers unless they clearly reduce complexity or
  create a real long-term seam.

## Upstream Awareness

Even though this branch is not upstream-bound, keep upstream replay in mind:
- When you learn something important about how a feature should be structured
  for upstream, note it.
- Prefer designs that will be easy to decompose into clean `pr/*` branches
  later, but don't let that slow you down here.
- If a quick hack is the right move to test an idea, do it — just mark it
  clearly so the clean version is obvious later.

## Upstream Conventions — Still Followed

We follow the project's own conventions even in this experimental branch,
because diverging from them makes later upstream replay harder.

### Coding style
- Tabs for indentation, 4-space tab width
- Pointer/reference binds to type: `Type* var`, not `Type *var`
- Formatting per `doc/coding-style.xml`

### Commit messages
Match the existing style. Pattern: `Component: Imperative description` or
`area: description`.

### C++ standard
C++14 is the project baseline.

### Qt6 forward compatibility
All new code must use only Qt6-safe APIs. No `QStringRef`, `QMatrix`,
`QRegExp`, or `QPrinter::PaperSize`.

### Build system
CMake. Keep the direct local build healthy.

## Build And Tooling

- Keep the direct local CMake build healthy while developing.
- Remember that upstream CI is superbuild-wrapped Azure, not just the local
  direct build.
- Local guidance files like this one should stay out of commits and PRs.
