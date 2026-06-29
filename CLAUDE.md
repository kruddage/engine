# KRUDD Engine — Agent Context

A game engine: C compiled to WASM via Emscripten, served as a static site.

## Architecture

```
modules/          C source modules (compiled into the main WASM module)
  core/           Engine heartbeat — init/tick/shutdown, subsystem manager,
                  plugin loader, ring buffer
  log/            Structured logging with level filtering and ring-buffer history
  memory/         Allocator and fixed-size pool allocator

plugins/          Dynamically-loaded WASM side modules
  include/        Public vtable headers (asset_api.h, entity_api.h,
                  backend_api.h, renderer.h, math_types.h, …)
  asset/          Asset catalog — enumeration, mutation, codec registration
  backend/        Persistence seam — Local provider with IndexedDB
  entity_plugin/  Runtime entity/scene system ("scene" subsystem)
  scene_plugin/   .scene v1 binary decoder (registered as asset codec)
  math/           vec3_t / quat_t / mat4 + camera helpers
  renderer/       GPU API vtable definition
  renderer_null/  Headless null renderer (used in native tests)
  renderer_webgl/ WebGL renderer (WASM only)
  frame_graph/    Frostbite-style render graph (GPU lent at execute time)
  imgui_plugin/   ImGui debug UI shell
  kruddboard/     In-browser tabbed authoring surface + markdown parser
  hello_plugin/   Minimal example plugin

docs/             Design documentation
  backend-abstraction.md  Local provider + IndexedDB persistence design
  entity-system.md        Struct-of-arrays runtime entity system
  frame-graph.md          Render graph architecture
  scene-format.md         Binary .scene v1 file format

CMakeLists.txt    Root build — emcmake cmake drives everything
build/            Build output (gitignored) — index.html, index.js, index.wasm
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

```sh
emcmake cmake -B build          # configure (requires Emscripten)
cmake --build build             # compile — outputs build/index.{html,js,wasm}
```

Serve `build/` with any static file server to run locally.

## Workflow

When you finish implementing a GitHub issue, create a pull request using the
GitHub MCP tools (`mcp__github__create_pull_request`). Reference the issue
number in the PR body (e.g. `Closes #N`).

## Key constraints

- C owns the loop. `engine_tick` is the frame callback passed to
  `emscripten_set_main_loop`. No JS/TS shell — Emscripten generates the glue.
- The plugin vtable headers in `plugins/include/` are the ABI. Changes to
  exported function signatures are breaking.
- Rendering is WebGL via `renderer_webgl`. No WebGPU yet.
- Native target compiles the modules and plugins for unit testing only; the
  engine loop does not run natively. The `#ifdef __EMSCRIPTEN__` guard in
  `main()` is the seam for a future native loop.
