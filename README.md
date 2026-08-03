# KRUDD

[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL--2.0--or--later-blue.svg)](https://spdx.org/licenses/GPL-2.0-or-later.html)
[![CI](https://github.com/kruddage/engine/actions/workflows/ci.yml/badge.svg)](https://github.com/kruddage/engine/actions/workflows/ci.yml)
[![Live](https://img.shields.io/website?url=https%3A%2F%2Fkruddage.github.io%2Fengine&label=live)](https://kruddage.github.io/engine)

A game engine written in C, compiled to WebAssembly via Emscripten and served as a static site.

**[Live demo →](https://kruddage.github.io/engine)**

## Overview

KRUDD is a modular C game engine that targets the browser via WebAssembly. The core drives a
fixed-timestep loop; subsystems (logging, memory, rendering) attach as plugins through a stable
WASM ABI.

Current state: entity/scene runtime, asset pipeline with local IndexedDB persistence,
and WebGL/WebGPU rendering with a frame graph. The page boots straight into a game; the
in-browser authoring surface is being reworked and is not wired up.

## Roadmap: Scheme as the build system and the game

KRUDD is mid strangler-fig rewrite. The end state: one small embedded language describes
both *how to build the game* and *what the game does*, so build rules and gameplay logic
stop being two separate disciplines.

**`kruddmake`** is that language — [S7 Scheme](https://ccrma.stanford.edu/software/snd/snd/s7.html)
embedded as the build driver. Scripts and scene/gameplay data authored in it use the
`.scm` extension; the underlying language stays plain S7 Scheme, not a fork, so existing
S7 docs and tooling keep applying.

Planned rollout:

1. **krudd drives the build.** The engine builds through krudd's own build (Ninja +
   Emscripten) documented above, inside CI. The gates around it (`.scm` comment lint,
   Conventional-Commit versioning via release-please, per-PR previews) still live as plain
   YAML workflows (see below). The sanitizer gate (ASan + UBSan + LeakSanitizer over the
   native suite) and a report-only coverage comment are wired up; a coverage *floor* gate
   is still deferred. The direction is to move that scaffolding into Scheme as the tooling
   exists, rather than growing it as a bolt-on to the old pipeline.
2. **krudd eats the build graph, piece by piece.** Asset codecs, plugin registration,
   and scene compilation move from C into Scheme one at a time — the C build tree shrinks
   as the Scheme grows, rather than a rewrite landing in one PR.
3. **The same S7 runtime ships in the engine** as the scripting layer for game logic, so a
   build script and a gameplay script share one language and one mental model.

Target experience: fork or clone this engine (or start from a release), push a branch,
merge to `main`, and GitHub Pages is running your game — no separate toolchain to learn.
Simple enough for a kid to poke at, deep enough not to be outgrown by someone who's shipped
AAA titles and HL1/Duke3D mods. This is a direction, not a shipped feature, and will keep
getting refined.

## Architecture

```
krudd/
  krudd.c        The front door — boots s7, hands off to the build language
  kruddmake/     The build language (kruddmake): reads specs, emits C + build.ninja, runs ninja
    build.scm    Orchestrator — the entry point `krudd build` loads
    manifest.scm The list of directories carrying a build.scm, in tier order
    ninja.scm    The Ninja emitter — renders build.ninja from the directory specs
    resolve.scm  Transitive include/link resolver
    introspect.scm Codegen — reads a module's .scm spec, emits its .h/.c
  engine/        The engine — one folder per module, Scheme spec + C together
    abi/         The plugin vtables, and nothing else
    base/        No engine concepts — log/, memory/, math/ (incl. the spatial types)
    core/        Engine heartbeat — init/tick/shutdown, subsystem manager, script host
    world/       The scene and its data model — entity/, asset/, edit/
    render/      Backends and the passes that drive them — webgl/, webgpu/,
                 null/, frame_graph/, particles/, scene_renderer/, plus renderer.scm
                 (the backend interface spec) and shader/ (the shader DSL)
    audio/       The mixer and its device backends
    ui/          The engine's own UI layer — kruddgui/, viewport/, kruddboard/
    game/        host/ is the launcher registry, project/ the generic host that
                 runs a game written as one (project ...) form
    shell/       The host the engine runs inside — web/
projects/        The games — chess/, training/, ducks/. One .scm each, and a sibling
                 of krudd/ rather than a module inside it: a project is content the
                 engine loads at runtime, not something it links
```

The tiers are listed in dependency order: a module may only reach for one in a tier above
it. `kruddmake/manifest.scm` is the authoritative list and explains what each tier is for,
and the build reads that list back: generating `build.ninja` fails on any `(library …
(link …))` edge that inverts it, naming both modules and their positions in the list.
Executables are exempt — nothing links one, so `core`'s `index` reaching for every backend
is the main module being assembled, not a tier reaching downward.

Each module owns its Scheme source-of-truth spec, the C it lowers to (or hand-written C for
speed), its headers, and its tests. A module whose Scheme is generated from — lowered to C,
embedded into the s7 image, substituted into a header — says so in its own `build.scm`, with
an `(embed …)` / `(emit-… )` / `(configure-file …)` declaration alongside its libraries. That
one declaration is what the generator runs *and* what the build watches for changes, so a
source can't be generated from without also being rebuilt for. A module that other modules
consume exports an
`include/` directory holding exactly what they consume; everything else stays private at
the module root. A module nothing outside consumes exports nothing — an empty surface is
the assertion that it is a leaf, not an oversight — and no module exports its own root.
Inside `include/` the headers sit one level down, in a directory named for the module, so
a public header is reached as `#include <webgpu/renderer_webgpu.h>` and a private one as
`#include "texture_registry.h"`. The prefix is the point: it puts the module boundary in
the source, where a reader sees a reach at the line that makes it, rather than only in the
build graph (`CODING_STANDARD.md`, "Public and private, and the two include forms"). `kruddmake/` is the thin build layer that reads those specs and emits +
compiles them; it holds no engine domain logic.

Every module is compiled straight into the one WASM module; at boot `engine.c` calls each
subsystem's `<name>_plugin_entry` in dependency order. A subsystem discovers engine services
through `subsystem_manager_get_api()` and interacts via vtables — no direct named imports
required.

## Building

### Prerequisites

- [Emscripten](https://emscripten.org/docs/getting_started/downloads.html) (emsdk) — WASM build
- [Ninja](https://ninja-build.org/) plus a C compiler (`cc`/`gcc`/`clang`)
- [Node](https://nodejs.org/) 20.11+ and [pnpm](https://pnpm.io/) — the workspace layer

krudd renders a `build.ninja` from the directory specs and drives `ninja`
directly — there is no CMake in the build path.

### The pnpm workspace

**The workspace is the physical design of the JavaScript layer, and kruddmake is
a deliberate second door: `krudd/kruddmake` builds C and WASM with a compiler and
nothing else, and the workspace never becomes a prerequisite for it.**
[`WORKSPACE.md`](WORKSPACE.md) is the long form — what the workspace is for,
where it stops, and why each of those was chosen over the alternatives.

The engine builds through a pnpm workspace that wraps kruddmake rather than
replacing it. `@kruddage/engine` spawns `krudd/kruddmake/kruddmake.sh` and
publishes the resulting artifacts behind a declared surface; everything
downstream reads that surface instead of the build tree. `krudd/` itself is not
in the workspace — it is a C tree with a shell entry point, and the one package
allowed to reach it by path is the one that wraps it.

`pnpm install` links the workspace and downloads nothing — it does not fetch
the toolchain below. `pnpm build` still needs everything in Prerequisites,
emsdk included; if a tool is missing it fails fast, naming what's missing
rather than building a partial WASM module.

```sh
corepack enable
pnpm install

pnpm build        # engine (WASM) then site, in dependency order
pnpm test         # the workspace's own suite — pure Node, no toolchain
pnpm test:native  # the native C suite, through the workspace — needs a compiler
pnpm check        # package boundaries
```

**Node is not a prerequisite for the native suite.** kruddmake is POSIX shell
and Scheme all the way down, and `sh krudd/kruddmake/run-tests.sh` builds and
runs the C tests with a compiler and nothing else — which is what the sanitizer
and coverage jobs invoke, and the path to reach for on a box with no Node. That
the workspace can also reach it, as `pnpm test:native`, is the second door —
named the same way in the root scripts, this README, and CI (WORKSPACE.md,
Q2).

| Package | What it is |
|---|---|
| [`@kruddage/engine`](packages/engine) | The engine's WASM build, harvested into `dist/` with a manifest describing it |
| [`@kruddage/site`](packages/site) | Stages the deployable static site from those artifacts (replaces `stage-site.sh`) |
| [`@kruddage/barriers`](tools/barriers) | The boundary check itself: `pnpm check` |
| [`@kruddage/render-diff`](tools/render-diff) | Screenshot oracle for the WebGPU port |
| [`@kruddage/dawn-smoke`](tools/dawn-smoke) | Proves a native Dawn build works offscreen; no `build` script, needs an out-of-tree Dawn install (`pnpm --filter @kruddage/dawn-smoke run smoke`) |

There are no third-party dependencies. `pnpm install` links the workspace and
downloads nothing, matching how the rest of the repo treats its supply chain
(vendored s7, a CDP client written against Node's built-in WebSocket, no CMake).

The point of the split is the boundary, not the packaging. `pnpm check` fails
the build on two things: a package reaching into another by relative path —
routing around the `exports` map — and anything but `@kruddage/engine` reaching
the build tree, by a path into `krudd/` or through the generator's environment.
A third rule, "only `@kruddage/engine` may depend on `@kruddage/kruddmake`",
went with the package it named when `krudd/` left the workspace; the path rule
already covered the same ground with the same exemption, and two mechanisms for
one rule is how the second one rots ([`WORKSPACE.md`](WORKSPACE.md), Q4). See
[`packages/engine/README.md`](packages/engine/README.md) for what the barrier
buys and where the next ones go.

### WASM build

```sh
pnpm --filter @kruddage/engine run build
```

Then serve the staged site with any static file server:

```sh
pnpm --filter @kruddage/site run build
python3 -m http.server -d packages/site/dist
```

kruddmake is still reachable on its own, and the workspace changes nothing about
what it does:

```sh
KRUDD_TARGET=wasm krudd/kruddmake/kruddmake.sh build   # -> build/
python3 -m http.server -d build
```

### Native build (tests only)

The native build compiles the modules for unit testing. It does not run the
engine loop; the test stamps run the suite, so a green build is a green test run.
It needs no emsdk and no Node.

```sh
krudd/kruddmake/kruddmake.sh build
```

`KRUDD_BUILD_DIR` points the generated `build.ninja` and its objects somewhere
other than `build/` — the same knob the sanitizer and coverage jobs use to keep
instrumented objects out of the plain build's tree.

A build leaves two directories at the root, both gitignored, and neither is a
`dist/`:

| Directory | What is in it |
|---|---|
| `build/` | kruddmake's output — the compiled artifacts, and for a WASM build the module and its staging inputs |
| `build-ninja/` | the generated `build.ninja` and the object files ninja builds from it |

The workspace convention right beside them is `packages/*/dist/`, which is where
each JS package's own output goes. These two are kruddmake's, they predate the
workspace, and `KRUDD_BUILD_DIR`, the sanitizer job's `build-san`, the coverage
job's `build-cov` and `.gitignore`'s `/build-*/` glob all key off the current
names — so they are documented rather than moved.

### The browser is the only target

KRUDD ships one artifact: the WASM module and the static site around it. There is no
desktop build, no installer, and no per-OS packaging — the authoring surface is the same
page the game runs in, so "install" is a URL.

The native build above exists to run the test suite on a build host, not to host the
engine: it compiles the modules and runs their tests, and never opens a window or drives
the engine loop. WebGL and WebGPU are the renderer backends; the `null` backend is what
the GPU-free tests record against.

## CI

`ci.yml` runs on every pull request and on push to `main`, alongside two release workflows:

| Workflow · job | What it does |
|---|---|
| **ci · lint** | Style-checks `.scm` comments (`lint-scm-comments.py`) and indentation; runs the workspace suite, the package-boundary check, and kruddmake's Scheme suite (which needs no compiler) |
| **ci · build** | Builds the WASM module via Emscripten (`emsdk` container) through krudd's own Ninja build, then stages the site — both through the pnpm workspace |
| **ci · deploy** | On push to `main`, publishes the staged site to GitHub Pages |
| **ci · preview** | Deploys each PR's build to a `pr-preview/pr-<N>/` URL and tears it down on close |
| **ci · sanitizers** | Builds + runs the native suite under ASan + UBSan + LeakSanitizer; fails on any leak, out-of-bounds, or UB |
| **ci · coverage** | Measures native gcov coverage and posts it as a sticky PR comment (report-only, no floor gate) |
| **pr-title** | Checks the PR title is a valid Conventional Commit (it becomes the squashed commit) |
| **release-please** | On push to `main`, maintains the release PR that versions, tags, and releases |

The `sanitizers` and `coverage` jobs both build natively through `kruddmake`, feeding the
sanitizer / `--coverage` flags in via the generator's `KRUDD_CC` / `KRUDD_EXTRA_CFLAGS` /
`KRUDD_EXTRA_LDFLAGS` environment hooks rather than as separate bolt-on build scripts. A
coverage *floor* gate isn't wired up yet — the plan is to add one once the baseline has
been watched for a while.

## Versioning and releases

[`version.txt`](version.txt) is the single source of the version, not `package.json` —
despite the pnpm workspace sitting at the root. `introspect.scm` stamps the number into
the WASM build and the shell template, and the site's cache-busting hash derives from what
that produces, so routing it through `package.json` would put Node in the path of a fact
the C build needs. Every `package.json` in the repo, including the workspace root, pins
`0.0.0`; that is not a stale placeholder, it is inert on purpose (see
[`WORKSPACE.md`](WORKSPACE.md), Q3).

Versioning is handled by [release-please](https://github.com/googleapis/release-please),
driven by [Conventional Commits](https://www.conventionalcommits.org/). We squash-merge, so a
PR's title *is* its commit message and the **pr-title** check enforces the format:

- `feat: …` → minor bump &nbsp;·&nbsp; `fix:`/`perf: …` → patch bump &nbsp;·&nbsp; `feat!:` or a `BREAKING CHANGE:` footer → major bump
- `chore:`/`docs:`/`ci:`/`refactor:`/`test:`/`build: …` → no version bump, but still recorded in `CHANGELOG.md`

On each push to `main`, release-please opens or updates a single **release PR** that rolls up
the unreleased commits: it bumps [`version.txt`](version.txt), regenerates `CHANGELOG.md`, and
updates `.release-please-manifest.json`. Merging that PR tags `vX.Y.Z` and cuts a GitHub
Release. CI reads `version.txt` and stamps it into the build (`KRUDD_VERSION`); PR/preview
builds append a `-pr<N>+<sha>` suffix so they never collide with a real release.

> This replaced an earlier scheme that derived the version by folding per-PR `release:*`
> labels on every build, with no tags or changelog. `version.txt` was seeded at the label-fold
> value (`17.11.3`) as of the cutover commit so numbering continues unbroken, and
> `bootstrap-sha` in the config keeps that pre-cutover history out of the first changelog.

## License

GPL-2.0-or-later for open source and GPL-compliant use. Use in proprietary or commercial products
requires a separate commercial license from the author.

External contributions require a CLA (copyright assignment). Contact the project maintainer for
details.

## Contributing

Code follows the [Linux kernel coding style](https://kernel.org/doc/html/latest/process/coding-style.html).
See [`CODING_STANDARD.md`](CODING_STANDARD.md) for the project-specific digest before writing or
reviewing any C.

Run `git config core.hooksPath .githooks` once to enable the tracked pre-commit hook, which mirrors
`ci·lint`'s `.scm` comment and indentation checks against your staged files so a violation is caught
locally instead of after a push.
