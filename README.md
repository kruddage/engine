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
WebGL rendering with a frame graph, and an in-browser authoring surface.

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
    manifest.scm The list of engine module directories
    ninja.scm    The Ninja emitter — renders build.ninja from the directory specs
    resolve.scm  Transitive include/link resolver
    introspect.scm Codegen — reads a module's .scm spec, emits its .h/.c
  engine/        The engine — one folder per module, Scheme spec + C together
    abi/         Public vtable headers (the plugin ABI)
    core/        Engine heartbeat — init/tick/shutdown, subsystem manager, script host
    log/         Structured logging with level filtering and ring-buffer history
    memory/      Allocator and fixed-size pool allocator
    math/        Vector/matrix math (math.scm spec → generated C) and camera
    render/      Rendering cluster — renderer interface spec + webgl/null backends,
                 frame_graph, scene_renderer
    shader/      The shader DSL + transpiler (shader.scm)
    asset/ entity/ edit/ ui/ …
                 Engine subsystems, all compiled into the single WASM module
```

Each module owns its Scheme source-of-truth spec, the C it lowers to (or hand-written C for
speed), its headers, and its tests. `kruddmake/` is the thin build layer that reads those
specs and emits + compiles them; it holds no engine domain logic.

Every module is compiled straight into the one WASM module; at boot `engine.c` calls each
subsystem's `<name>_plugin_entry` in dependency order. A subsystem discovers engine services
through `subsystem_manager_get_api()` and interacts via vtables — no direct named imports
required.

## Building

### Prerequisites

- [Emscripten](https://emscripten.org/docs/getting_started/downloads.html) (emsdk) — WASM build
- [Ninja](https://ninja-build.org/) plus a C compiler (`cc`/`gcc`/`clang`)

krudd renders a `build.ninja` from the directory specs and drives `ninja`
directly — there is no CMake in the build path.

### WASM build

```sh
KRUDD_TARGET=wasm ./krudd.sh build
```

Then serve `build/` with any static file server:

```sh
python3 -m http.server -d build
```

### Native build (tests only)

The native build compiles the modules for unit testing. It does not run the
engine loop; the test stamps run the suite, so a green build is a green test run.

```sh
./krudd.sh build
```

### Native editor (SteamOS / Steam Deck)

A native editor: the same C engine that ships to the browser as WebAssembly also runs
natively on a **Vulkan** backend (Vulkan on the Deck's RDNA2, on Windows too) presenting
into a real desktop window — no browser, no Emscripten in the path. The web build keeps
its WebGL and WebGPU backends untouched; Vulkan is the native desktop GPU path.

The Vulkan backend brings up a **modern Vulkan 1.3 device with the Khronos validation
layers on** and presents an animated clear into the window, and the editor still boots the
engine's render cluster (asset → entity → frame graph → scene renderer) so the whole path
up to the backend runs. Translating that draw stream to Vulkan — shaders to SPIR-V,
pipelines, buffers and draws, so the demo scene renders in Vulkan — is the next step; the
point of this stage is a validated Vulkan base you can diagnose against on real hardware
(see [#705](https://github.com/kruddage/engine/issues/705)).

The editor is the **Qt editor shell** — a `QMainWindow` with a menu bar, toolbar and
Scene/Inspector/Assets/Console docks around the viewport. It is opt-in and left out of
every default build and CI run:

```sh
KRUDD_QT_CFLAGS="$(pkg-config --cflags Qt6Widgets Qt6Gui Qt6Core)" \
./krudd.sh editor
```

It needs the **Vulkan loader + headers + validation layers**, **glslang**, and **Qt6** —
all ordinary system packages, no multi-gigabyte out-of-tree library to build. On the Deck,
SteamOS's root filesystem is immutable, so build and run inside an Arch
[distrobox](https://distrobox.it/) (it shares the Deck's Wayland socket and GPU). To get
going from a clean checkout:

```sh
git clone https://github.com/kruddage/engine.git
cd engine
./setup.sh          # toolchain + Vulkan validation layers + Qt6 + .krudd-env
./krudd.sh editor   # build and run the Qt editor shell
```

`setup.sh` records the Qt flags in `.krudd-env`, which `krudd.sh` sources automatically — so
after `./setup.sh` you do not even need the manual `KRUDD_QT_CFLAGS` export shown above.

The complete setup — the Vulkan prerequisites, Wayland vs X11, and what you should see on
screen — lives in [`docs/qt-editor-shell.md`](docs/qt-editor-shell.md).

The editor also ships as a self-hosted, GPG-signed Flatpak registry —
`flatpak remote-add` a `.flatpakrepo` URL and get updates the normal Flatpak
way, no Flathub submission. See
[`packaging/flatpak/`](packaging/flatpak/README.md) for install instructions
and how to stand up your own signed registry on a fork; it's the same
`gh-pages` deploy the WASM site already uses, just published to a `/flatpak/`
subpath. Wiring the editor's docks to the running scene (scene tree, inspector,
REPL, project open/save) is tracked separately as the authoring surface.

## CI

`ci.yml` runs on every pull request and on push to `main`, alongside two release workflows:

| Workflow · job | What it does |
|---|---|
| **ci · lint** | Style-checks `.scm` comments (`lint-scm-comments.py`) |
| **ci · build** | Builds the WASM module via Emscripten (`emsdk` container) through krudd's own Ninja build |
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
