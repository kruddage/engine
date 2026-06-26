# KRUDD

[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL--2.0--or--later-blue.svg)](https://spdx.org/licenses/GPL-2.0-or-later.html)
[![CI](https://github.com/kruddage/engine/actions/workflows/ci.yml/badge.svg)](https://github.com/kruddage/engine/actions/workflows/ci.yml)
[![Sanitizers](https://github.com/kruddage/engine/actions/workflows/sanitizers.yml/badge.svg)](https://github.com/kruddage/engine/actions/workflows/sanitizers.yml)
[![clang-tidy](https://github.com/kruddage/engine/actions/workflows/clang-tidy.yml/badge.svg)](https://github.com/kruddage/engine/actions/workflows/clang-tidy.yml)
[![Coverage](https://github.com/kruddage/engine/actions/workflows/coverage.yml/badge.svg)](https://github.com/kruddage/engine/actions/workflows/coverage.yml)
[![Live](https://img.shields.io/website?url=https%3A%2F%2Fkruddage.github.io%2Fengine&label=live)](https://kruddage.github.io/engine)

A game engine written in C, compiled to WebAssembly via Emscripten and served as a static site.

**[Live demo →](https://kruddage.github.io/engine)**

## Overview

KRUDD is a modular C game engine that targets the browser via WebAssembly. The core drives a
fixed-timestep loop; subsystems (logging, memory, rendering) attach as plugins through a stable
WASM ABI.

Current state: heartbeat loop with logging, memory management, and a plugin loader. WebGL
rendering is in active development.

## Architecture

```
modules/
  core/      Engine heartbeat — init/tick/shutdown, subsystem manager, plugin loader
  log/       Structured logging with level filtering and ring-buffer history
  memory/    Allocator and fixed-size pool allocator
```

Plugins are dynamically loaded WASM modules. Each plugin discovers engine services through
`subsystem_manager_get_api()` and interacts via vtables — no direct named imports from the main
module required.

## Building

### Prerequisites

- [Emscripten](https://emscripten.org/docs/getting_started/downloads.html) (emsdk)
- CMake ≥ 3.x

### WASM build

```sh
emcmake cmake -B build
cmake --build build
```

Then serve `build/` with any static file server:

```sh
python3 -m http.server -d build
```

### Native build (tests only)

The native build compiles the modules for unit testing and static analysis. It does not run the
engine loop.

```sh
cmake -B build_native
cmake --build build_native
ctest --test-dir build_native --output-on-failure
```

## CI

| Workflow | What it checks |
|---|---|
| **CI** | WASM build via Emscripten |
| **Sanitizers** | ASan + UBSan build; full test suite |
| **Coverage** | Line coverage enforced at ≥ 80% |
| **clang-tidy** | Static analysis on all non-test source files |
| **PR preview** | Deploys each PR to a GitHub Pages preview URL and posts the link |

## License

GPL-2.0-or-later for open source and GPL-compliant use. Use in proprietary or commercial products
requires a separate commercial license from the author.

External contributions require a CLA (copyright assignment). Contact the project maintainer for
details.

## Contributing

Code follows the [Linux kernel coding style](https://kernel.org/doc/html/latest/process/coding-style.html).
See [`CODING_STANDARD.md`](CODING_STANDARD.md) for the project-specific digest before writing or
reviewing any C.
