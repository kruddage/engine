# KRUDD Engine — Agent Context

A game engine: C modules compiled to WASM, driven by a TypeScript shell in the browser.

## Architecture

```
modules/          C source modules, each compiled to a WASM binary
  core/           First module — engine ABI and proof-of-life
    engine.h      Public C ABI (the stable contract)
    engine.c      Implementation
    Makefile       Compiles to public/wasm/core.wasm

public/wasm/      Compiled WASM binaries (built artifacts, served as static files)

shell/src/        TypeScript host
  loop.ts         EngineLoop — rAF tick, update/render callbacks
  main.ts         Entry point — loads WASM, drives the loop
```

## C coding standard

All C code follows the Linux kernel coding style. See `CODING_STANDARD.md` at the
repo root before writing or reviewing any C. Short version: tabs, 8-column indent,
80-column lines, K&R braces, snake_case, `/* */` comments, fixed-width types at
ABI boundaries.

## Building

```sh
npm run build:wasm   # compile all C modules to WASM (requires clang + wasm-ld)
npm run build        # build:wasm + tsc check + vite bundle
npm run dev          # vite dev server — run build:wasm first
npm test             # vitest
```

WASM binaries are not committed to git. CI builds them from source via GitHub
Actions (`.github/workflows/deploy.yml`) before deploying to Cloudflare Pages.
Run `npm run build:wasm` locally once before `npm run dev`.

## Key constraints

- The C header (`.h`) is the ABI. Changes to exported function signatures are
  breaking changes — treat them as such.
- WASM binaries live in `public/wasm/` and are committed so the site deploys
  without a native toolchain in the CI environment.
- TypeScript shell code talks to WASM exports only — never to Canvas2D or WebGPU
  directly in the long run; the renderer will be a WASM module too.
