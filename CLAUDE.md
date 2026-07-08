# KRUDD Engine — Agent Context

A game engine: C compiled to WASM via Emscripten, served as a static site.

## Architecture

```
krudd/                    The build tool and the whole build tree it owns —
                          krudd/<command> per command; `build` is the only one
                          so far
  krudd.c                 The orchestrator, built by krudd.sh
  build/                  Everything the `build` command owns
    build.scm             Embedded s7 Scheme: reads the per-directory build.scm
                          specs below, renders a build.ninja (ninja/ninja.scm),
                          and drives ninja with cc / emcc directly — no CMake
    introspect.scm        Build-time introspection krudd owns: VERSION + git
                          facts, configure_file / embed codegen, dep fetch
    ninja/                The build tree: the Ninja emitter plus the C sources
                          it renders. Each owned directory carries a build.scm
                          spec (a backend-agnostic list of target forms) beside
                          its sources.
      ninja.scm           The emitter — renders build.ninja from the specs
      resolve.scm         Transitive include/link resolver (+ its tests)
      manifest.scm        The list of owned directories
      modules/            C source modules (compiled into the main WASM module)
        core/             Engine heartbeat — init/tick/shutdown, subsystem
                          manager, ring buffer, and the embedded s7 Scheme
                          runtime (script.c + runtime.scm) that owns the body
                          of the frame
        log/              Structured logging with level filtering and
                          ring-buffer history
        memory/           Allocator and fixed-size pool allocator
      plugins/            Engine plugins — each registers a subsystem through a
                          vtable. All compiled into the single WASM module (no
                          dynamic loading); engine.c calls each plugin's
                          <name>_plugin_entry at boot in dependency order
        include/          Public vtable headers (asset_api.h, entity_api.h,
                          backend_api.h, renderer.h, math_types.h, …)
        asset/            Asset catalog — enumeration, mutation, codec
                          registration; built-in primitive geometry
                          (primitives.c)
        backend/          Persistence seam — Local provider with IndexedDB
        entity_plugin/    Runtime entity/scene system ("scene" subsystem)
        scene_plugin/     .scene v1 binary decoder (registered as asset codec)
        math/             vec3_t / quat_t / mat4 + camera helpers
        renderer/         GPU API vtable definition
        renderer_null/    Headless null renderer (used in native tests)
        renderer_webgl/   WebGL renderer (WASM only)
        frame_graph/      Frostbite-style render graph (GPU lent at execute time)
        scene_renderer/   Draws COMPONENT_RENDER entities via the frame graph (#172)
        imgui_plugin/     ImGui debug UI shell
        kruddboard/       In-browser tabbed authoring surface + markdown parser

docs/                     Design documentation
  backend-abstraction.md  Local provider + IndexedDB persistence design
  entity-system.md        Struct-of-arrays runtime entity system
  frame-graph.md          Render graph architecture
  scene-renderer.md       Entity rendering through the frame graph
  scene-format.md         Binary .scene v1 file format
  scheme-runtime.md       Embedded s7 Scheme runtime — Scheme owns the frame

build/                    Build output (gitignored) — index.html, index.js, index.wasm
```

## License

All source files are licensed under **GPL-2.0-or-later**. Every new `.c` or `.h`
file must begin with:

```c
/* SPDX-License-Identifier: GPL-2.0-or-later */
```

Do not use MIT, LGPL, or any other identifier. The project was MIT up to tag
`last-mit-release`, then LGPL-2.1-or-later briefly, and is now GPL-2.0-or-later.
Use of the engine in proprietary commercial products requires a commercial license
from the author. Open source / GPL-compliant use is free. This dual model requires
copyright assignment via CLA for external contributions.

## C coding standard

All C code follows the Linux kernel coding style. See `CODING_STANDARD.md` at the
repo root before writing or reviewing any C. Short version: tabs, 8-column indent,
80-column lines, K&R braces, snake_case, `/* */` comments, fixed-width types at
ABI boundaries.

## Building

krudd renders a `build.ninja` from the directory specs and drives `ninja`
directly — no CMake. Requires `ninja` plus a C compiler (native) or the
Emscripten toolchain on `PATH` (WASM).

```sh
./krudd.sh build                 # native: builds the static libs and runs the
                                 # test suite (test stamps run the tests)
KRUDD_TARGET=wasm ./krudd.sh build   # WASM: outputs build/index.{html,js,wasm}
                                 # — one module, all plugins folded in (needs
                                 # emcc/em++)
```

Serve `build/` with any static file server to run the WASM build locally.

## Workflow

When you finish implementing a GitHub issue, create a pull request using the
GitHub MCP tools (`mcp__github__create_pull_request`). Reference the issue
number in the PR body (e.g. `Closes #N`).

## Versioning

The `VERSION` file is the single source of truth for the engine version;
`modules/core/version.h.in` is expanded against it at build time. There is no
`CHANGELOG.md` — release notes are not tracked in-tree.

Every PR carries exactly one `release:feature` / `release:fix` /
`release:breaking` / `release:chore` label, enforced by the release-label-gate
CI check. Labels are defined in `.github/labels.yml` and synced to the repo by
a workflow — don't create or edit them by hand in the GitHub UI.

## Key constraints

- C lends the loop; Scheme owns the frame body. `engine_tick` is still the
  callback passed to `emscripten_set_main_loop`, but it delegates the frame to
  the embedded s7 runtime's `(tick)` (see `docs/scheme-runtime.md`). No JS/TS
  shell — Emscripten generates the glue.
- The plugin vtable headers in `krudd/build/ninja/plugins/include/` are the ABI.
  Changes to exported function signatures are breaking.
- Rendering is WebGL via `renderer_webgl`. No WebGPU yet.
- Native target compiles the modules and plugins for unit testing only; the
  engine loop does not run natively. The `#ifdef __EMSCRIPTEN__` guard in
  `main()` is the seam for a future native loop.
