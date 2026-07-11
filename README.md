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
WebGL rendering with a frame graph, and an in-browser authoring surface (kruddboard).

## Roadmap: Scheme as the build system and the game

KRUDD is mid strangler-fig rewrite. The end state: one small embedded language describes
both *how to build the game* and *what the game does*, so build rules and gameplay logic
stop being two separate disciplines.

**`kruddmake`** is that language — [S7 Scheme](https://ccrma.stanford.edu/software/snd/snd/s7.html)
embedded as the build driver. Scripts and scene/gameplay data authored in it use the
`.kscm` extension; the underlying language stays plain S7 Scheme, not a fork, so existing
S7 docs and tooling keep applying.

Planned rollout:

1. **krudd drives CI.** A single Ubuntu + S7 job builds the engine through krudd's own
   build (Ninja + Emscripten) documented above — no separate lint/sanitizer/coverage
   jobs yet. Those were deliberately stripped from `ci.yml` (see below) and come back
   written in Scheme once the tooling exists, rather than as a bolt-on to the old pipeline.
   This is a temporary trade-off, not a claim that those checks were unnecessary.
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
    shader/      The shader DSL + transpiler (shader.scm) and node-graph reader (dag.scm)
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

## CI

| Workflow | What it checks |
|---|---|
| **CI** | WASM build via Emscripten; on push to `main`, also publishes to GitHub Pages |

This is intentionally bare-bones: the previous lint/sanitizer/coverage gates, PR previews,
and changelog/release-label gates were stripped down to this single job to make room for the
`kruddmake` rewrite (see Roadmap above) — they're expected to return via `kruddmake` rather
than as separate bolt-on workflows.

## License

GPL-2.0-or-later for open source and GPL-compliant use. Use in proprietary or commercial products
requires a separate commercial license from the author.

External contributions require a CLA (copyright assignment). Contact the project maintainer for
details.

## Contributing

Code follows the [Linux kernel coding style](https://kernel.org/doc/html/latest/process/coding-style.html).
See [`CODING_STANDARD.md`](CODING_STANDARD.md) for the project-specific digest before writing or
reviewing any C.
