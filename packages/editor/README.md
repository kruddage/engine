# @kruddage/editor

The KRUDD editor, as a TypeScript application.

```sh
pnpm --filter @kruddage/editor run dev        # the editor, against a built engine
pnpm --filter @kruddage/editor run test       # the component suite — no browser, no emsdk
pnpm --filter @kruddage/editor run test:e2e   # the browser suite — needs a built engine
pnpm --filter @kruddage/editor run build      # typecheck, then a production build
```

## What this is, at this stage

**The package and its plumbing, and nothing that looks like an editor yet.**
There are no docks, no panels and no command bar here. That is
[#954](https://github.com/kruddage/engine/issues/954), which builds the shell on
top of what this package proves works.

What it proves is [#946](https://github.com/kruddage/engine/issues/946)'s claim:
that the editor can be a normal React application that consumes the engine's
WASM artifact, builds, tests and ships through the same workspace as everything
else. `src/app.tsx` renders the engine's version, its WASM exports, a canvas and
a live status strip — and it is written to be deleted when the shell arrives.

## The end of zero-dep

`pnpm-lock.yaml` used to be 23 lines. Its only entry was a `workspace:*` link,
`pnpm install` downloaded nothing, and that was a real position — consistent
with vendored s7, a hand-written CDP client and a build system written in
Scheme.

**This package ended it, on purpose.** A dock splitter, a virtualized tree, a
node canvas and a gesture layer are each months of work the ecosystem has
already done, and hand-rolling them buys nothing an editor's users will ever
see. The rule going forward is #944's: reach for the package, write down what it
is for, move on.

Nothing else changed. The engine still builds through kruddmake with a compiler
and nothing else, `version.txt` is still the only source of the version, and no
package that feeds the C build acquired a registry dependency.

### Every dependency, and what replacing it would cost

The list is short on purpose, and it is short because this PR deliberately did
**not** add the packages #946 names for later work. `react-resizable-panels`,
`@xyflow/react`, `react-arborist`, an accessible-primitives library and
`@use-gesture/react` all arrive with the issues that use them — adding them here
would mean five unused dependencies whose justifications nobody could check
against real code.

| Package | What it does here | Replacing it |
|---|---|---|
| `react` / `react-dom` | The UI runtime. The substrate decision from #944, not a preference | A rewrite of the editor |
| `vite` | Dev server with fast refresh, and the production build | Weeks, and worse. This is the "not the place to be interesting" choice |
| `@vitejs/plugin-react` | JSX transform and fast refresh for Vite | Nothing else does this job for this bundler |
| `typescript` | Strict mode, and the build fails on a type error | The `.ts` in "the editor is a TypeScript application" |
| `vitest` | The component suite (#944, Q6) | A day to move to `node:test` + a DOM shim, and the shim is what Q6 rejected |
| `jsdom` | The DOM Vitest runs components against | Interchangeable with `happy-dom`; a config line |
| `@testing-library/react` + `@testing-library/dom` | Rendering and querying components in tests | Hand-rolled render helpers — a few hundred lines that drift |
| `@playwright/test` | The browser suite: real Chromium, real WASM | The vendored CDP client plus fixtures, retries, tracing and parallel workers. Considered and rejected in the Q6 decision — that is maintaining a browser-automation framework as a side effect of building an editor |
| `@types/node`, `@types/react`, `@types/react-dom` | Types for the above | Nothing; they are types |
| `@kruddage/engine` | Where the WASM artifact is. A `workspace:*` link, and a **dev** dependency — it is consulted at build time and none of it is bundled | It is the boundary; there is nothing to replace it with |

## How the editor meets the engine

**It consumes the artifact. It never produces one.** kruddmake stays the only
thing that compiles C, and `version.txt` stays the only source of the version —
routing either through Node would put the JS toolchain in the path of a fact the
C build needs (see the root `package.json`'s `//version` note).

`build/engine-artifacts.mts` is the whole of the seam. It asks
`@kruddage/engine` where the outputs are rather than knowing, which is what the
workspace boundary check enforces and the reason that package exists at all. It
does three things:

- **dev** — serves the artifacts under `/engine/`, so `pnpm dev` runs against a
  real engine with fast refresh over the top
- **build** — copies the same files into `dist/engine/`, so what ships is what
  was tested
- **both** — exposes the manifest to the app as `virtual:krudd-engine`, so the
  editor can render the engine's version without a fetch and without a second
  copy of the artifact contract

### The cache-busting stem, deliberately absent

The deployed site renames `index.wasm` to `index.<hash>.wasm`, and the generated
shell finds it only because the same hash was baked into a `Module.locateFile`
hook at build time. Two derivations of one hash in two languages that have to
agree — `@kruddage/site` cross-checks them for exactly that reason.

**The editor sidesteps the mechanism entirely**: it ships the artifacts under
their real names, so its `locateFile` is the identity. That is not an oversight,
and it must not be "fixed" by copying the shell's version — a stem applied to
files that were never renamed asks the browser for a URL that was never staged,
and it fails at runtime and nowhere else. `test/boot.test.ts` guards it.

### An unbuilt engine is a supported state

`pnpm --filter @kruddage/engine run build` needs emsdk, and a contributor
working on the editor's chrome may not have it. Rather than failing, the plugin
reports `built: false` and the app says so on screen with the command that fixes
it. The component suite does not care either way.

What does **not** happen is pretending: `scripts/e2e.mjs` refuses to run the
browser suite against an engine that is not there, rather than skipping quietly.
A skipped browser suite reports green, and a green suite that never booted a
module is precisely what #946 warned about.

## Where the page comes from

**Beside the existing shell, at its own route — not replacing it.**

The generated shell is stamped by the C build: the version, the cache-busting
hook and the Scheme-derived chrome all arrive from kruddmake. Replacing it would
mean moving those into Vite, which is the coupling the root `package.json`
already forbids. Mounting into it would mean the React app inheriting a DOM
another system builds at boot.

Sitting beside it costs nothing, keeps the existing shell working while the
editor is a skeleton, and is what lets
[#953](https://github.com/kruddage/engine/issues/953) retire the old chrome as a
deliberate step rather than as this PR's side effect. `base` is `"./"` so the
same output works at `/editor/` on the deployed site, at `/` under `vite
preview`, and under a preview deploy's PR-number prefix, without a second build.

## How this is tested

[#944's Q6](https://github.com/kruddage/engine/issues/944) decided it: **Vitest
for components, Playwright for the browser.** Both are set up here and each is
proven with real tests, because a configured-but-unused runner decides nothing.

| | What runs there | Where in CI |
|---|---|---|
| **Vitest** (`pnpm test`) | Components, hooks, the boot path up to the module itself, the build plugin's path handling | The `workspace` job — no emsdk, no browser |
| **Playwright** (`pnpm test:e2e`) | The engine actually booting: WASM served as `application/wasm`, `main()` running, a live renderer reported | The `build` job, after the engine is built |

**The rule: a test goes in Playwright when it would lie in Vitest, and not
because it feels more real.** A browser suite that grows to cover what jsdom
could have answered is how a fast suite becomes one nobody runs.

`pnpm test` deliberately does not invoke Playwright. The root `pnpm test` runs
in a job with no emsdk, so there is no WASM for a browser to boot; wiring them
together would make the fast suite depend on a toolchain it does not need.

### Two configs, two typechecks

`tsconfig.json` is the application — `src/` and `test/`, and **no Node types**,
so a stray `node:fs` in browser code is a type error rather than something Vite
discovers at bundle time. `tsconfig.node.json` is the build-time half: the Vite
plugin, the scripts and the three configs. `pnpm typecheck` runs both, and
`build` runs `typecheck` — Vite transpiles without checking and would otherwise
ship a type error happily.

## What is deliberately not here

- **The shell** — docks, panels, the command bar, the panel vocabulary. #954
- **The boundary** — reading the scene, mutating it, hearing about changes.
  #945. Nothing in this package is one, and `src/engine/boot.ts` says so out
  loud so it is not mistaken for one later
- **The node graph** — #944's Q5
- **The five UI packages** — they arrive with the issues that use them
