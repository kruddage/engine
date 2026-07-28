# @kruddage/engine

The KRUDD engine, as an npm package.

The engine is not ported. It is still C, still built by
[kruddmake](../../krudd/kruddmake) — the s7 Scheme build system that renders a
`build.ninja` from per-directory specs and drives `emcc` through it. This
package wraps that build and puts a declared surface in front of its output.

```sh
pnpm --filter @kruddage/engine run build        # needs emsdk on PATH
pnpm --filter @kruddage/engine run test         # this package's own suite, pure Node
pnpm --filter @kruddage/engine run test:native  # the C suite, no emsdk needed
```

## What it does

`scripts/build.mjs` runs `kruddmake.sh build` with `KRUDD_TARGET=wasm`, then
harvests the outputs into `dist/` alongside an `engine-manifest.json` that
describes them: version, the cache-busting stem, the artifact list with sizes
and SHA-256s, and the function exports read back out of the WASM module.

It reaches kruddmake by path. `scripts/kruddmake.mjs` derives the repo root from
its own location and joins `krudd/kruddmake/` onto it, and it is the only file
in this package that does — every other module here asks it.

That path is what `pnpm check` rule 3 forbids to every package but this one, and
the exemption is the whole of the permission. It used to be the second of two
gates: `krudd/kruddmake` was a package, this one declared a dependency on it,
and a separate rule said no other package could. That rule went with the package
when `krudd/` left the workspace (#934) — its ground was already covered by the
path rule, under the same exemption, and two mechanisms for one rule is how the
second one rots. The honest consequence is that the exemption now carries the
barrier alone: there is one line between `krudd/` and the rest of the workspace,
and it is the name check in `scripts/check-barriers.mjs`.

Consumers import from the package, never from the build tree:

```js
import { artifacts, readManifest, assetDir } from "@kruddage/engine";

const { version, cacheStem, wasmExports } = readManifest();
for (const a of artifacts()) console.log(a.role, a.name, a.path);
```

## Why the artifact contract lives here

`src/artifacts.mjs` records more than a file list. It records which outputs may
be renamed for cache-busting and which references move with them — and those are
facts only the engine is in a position to know:

- `index.wasm` is renamed at deploy time, and that works **only** because the
  emscripten shell template bakes a `Module.locateFile` hook carrying the same
  commit hash (`shell/web/shell.html.in`). The engine build is what makes that
  true, so the engine is what declares it.
- `index.wasm` is nonetheless *not* rewritten inside `index.html`. The one
  literal `index.wasm` in the document is the error overlay's display text; the
  real URL is produced at runtime by that hook. Renaming the text would show
  users a filename that does not exist.
- `sw.js`, `manifest.webmanifest` and the icons keep their names because the
  page and the browser reference them literally.

Before this package, that knowledge lived in `.github/scripts/stage-site.sh`,
which re-derived the hash with its own `git rev-parse --short HEAD` and trusted
it to match what the Scheme had independently baked in. Two derivations that had
to agree, in two languages, with nothing checking that they did — and a mismatch
showed up only as a 404 on the live site. The stem now comes out of the built
HTML, so there is one derivation and the staging step reads it.

## What this package is not

It does not export a JS API onto the engine. The engine is a single emscripten
link unit — one `index.js` glue file and one `index.wasm`, linked without
`-sMODULARIZE` — so there is no finer seam to hand out that would not be
invented. What is published is the artifact set, its metadata, and the real
export surface.

`declaredExports` in the manifest mirrors `-sEXPORTED_FUNCTIONS` from
`ninja.scm`; `wasmExports` is what `src/wasm-exports.mjs` actually found in the
module. They are recorded side by side rather than asserted equal, because
emscripten is free to rename an entry point on the way out — a `main` taking
`argv` comes through as `__main_argc_argv`. Once CI has shown what the real
names are, the two can be tied together into a gate.

## The next barriers

This is the first membrane, not the finished shape. In rough order of value:

1. **A modularised loader.** Building with `-sMODULARIZE=1 -sEXPORT_ES6=1` turns
   `index.js` into an ES module, at which point this package can export a real
   `createEngine()` instead of a file list, and the shell stops being the only
   thing that knows how to boot the engine.
2. **Move the web shell out.** `shell/web/` is HTML, JS, icons and a manifest
   living inside the C tree because the emcc link needs the shell template as an
   input. Splitting it into `@kruddage/shell-web` — with the template staying an
   engine build input — would let the page evolve without touching kruddmake.
3. ~~**Split the ABI.**~~ Done, on the C side, which was the half that mattered.
   `krudd/engine/abi/` declares `(interface-library "abi" (interface "."))` in
   its `build.scm` and sits first in `manifest.scm`: it compiles nothing, it is
   headers every tier includes and nothing links, and the tier check reads that
   position back (#919). It briefly also had a `package.json` and was the first
   C module in the pnpm workspace. That half is reverted — see below.
4. ~~**Package the build system.**~~ Reverted, deliberately, and what it was for
   was kept. `krudd/kruddmake/kruddmake.sh` is the build entry point, and
   `run-scheme-tests.sh` is a suite that runs on the s7 CLI with no compiler;
   both landed with #920 and both stay. The `package.json` around them does
   not (#934). Packaging the C tree meant 2 of its 23 modules had manifests
   with no plan for the other 21, which is a worse state than none — a reader
   cannot tell whether the rest are pending or excluded — and the barrier it
   bought is enforced by path instead. Next of these is the shader transpiler,
   which needs neither a compiler nor the engine (#921); it is JS, so it is a
   package without any of that argument applying.

Each step is independently useful, and none of them require porting the engine.
