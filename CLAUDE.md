# KRUDD Engine — Agent Context

A game engine: C compiled to WASM via Emscripten, served as a static site.

## Architecture

```
modules/          C source modules
  core/           First module — engine heartbeat
    engine.h      Public C ABI
    engine.c      Implementation — main(), engine_init/tick/shutdown

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
- The `.h` file is the ABI. Changes to exported function signatures are breaking.
- No WebGPU yet — rendering comes later. Current loop is a pure heartbeat.
- Native target is planned but not yet implemented. The `#ifdef __EMSCRIPTEN__`
  guard in `main()` is the seam for a future native loop.
