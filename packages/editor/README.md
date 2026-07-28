# @kruddage/editor

The KRUDD editor, as a TypeScript application.

```sh
pnpm --filter @kruddage/editor run dev        # the editor, against a built engine
pnpm --filter @kruddage/editor run test       # the component suite — no browser, no emsdk
pnpm --filter @kruddage/editor run test:e2e   # the browser suite — needs a built engine
pnpm --filter @kruddage/editor run build      # typecheck, then a production build
```

## What this is, at this stage

**The shell, and panels that are honest about being empty.** Command bar,
toolbar, rail, resizable docks around a viewport deck, and a status strip —
built in [#954](https://github.com/kruddage/engine/issues/954) as far as it goes
without the boundary, which turns out to be all the way, because none of the
panels have contents yet.

They are empty in the shipped editor today too. Every dock in
`krudd/engine/core/editor_layout.scm` renders a heading and a one-line blurb;
this shell renders the same headings and the same blurbs, plus the issue that
fills each one in. What it adds is a frame the later panels mount into rather
than replace.

### The vocabulary

Settled in `src/shell/vocabulary.ts`, one line each, and checked by the suite so
it cannot quietly grow a synonym:

**command bar, toolbar, rail, viewport deck, outliner, inspector, assets,
console, build, status strip.**

Use these words in code, comments, commit messages and PR bodies. "Sidebar" is
not a rail and "tree" is not the outliner — a second name for one thing is how
six PRs end up describing four different layouts. #855 fixed the board's
vocabulary the same way and it is why board/node/wire/port/lane never became an
argument.

### A panel registers itself

```ts
registerPanel({ id: "outliner", title: "Outliner", home: "left", render: () => <Outliner /> });
```

The shell reads the registry and places what it finds, so
[#950](https://github.com/kruddage/engine/issues/950),
[#951](https://github.com/kruddage/engine/issues/951) and
[#952](https://github.com/kruddage/engine/issues/952) each add a file and a line
to `src/panels/index.tsx` and touch no part of the frame. `test/shell.test.tsx`
asserts it, because that criterion is the difference between three small PRs and
three PRs that each edit the layout everything else depends on.

**A panel is mounted once and moves.** There is no path in the shell that
constructs a second instance of one — #886's mockup grew three Inspectors that
drifted apart, and that is the outcome this rule makes impossible rather than
merely discouraged.

### The layout is remembered, and a stale one is survivable

Docks resize, collapse and restore; the arrangement persists across a reload and
View > Reset Layout puts it back. The interesting half is `reconcile()` in
`src/shell/layout.ts`: a reader's stored layout is always older than the build
reading it, so a panel that no longer exists is dropped, a panel the store never
heard of is placed at its home, and an unreadable store falls back to the
default. A shell that crashed on any of those is one a reader fixes by clearing
site data, which they will not know to do.

### What is wired, and what says "coming soon"

`reset-layout`, the panel toggles and the moves are real. **Every other menu
action shows a transient hint** — the label with its `&` mnemonic stripped, then
": coming soon" — which is exactly what `shell.html.in:1548` does today and what
the native host does for an id it does not recognise. That is what makes a
read-only shell honest rather than broken, and
[#948](https://github.com/kruddage/engine/issues/948) replaces the hint path
with #947's command layer without moving anything.

The menu set, the labels and the shortcuts were **mined from
`editor_layout.scm` and then the file was closed**. Nothing reads it at runtime
(#944's Q3); `test/commands.test.ts` carries the spec transcribed by hand so a
divergence is a decision someone made rather than one the suite let through.

### Container queries, not media queries

A panel answers to the frame it is in, not to the window, so a dock dragged
narrow reflows while the window stays wide. Every dock body is a query
container. #944 rules the phone out of scope and this goes in anyway: it costs
days now and a rewrite of every panel later. `test/styles.test.ts` fails the
build on a width media query, and on any font size fixed in pixels.

### No boundary work

Not one new engine export, no new `EM_JS`, and nothing in this package calls
`@kruddage/engine/bridge` yet. The status strip's renderer badge and the
console's scrollback come from push channels that already existed; fps and the
resolution are measured from the page, and `src/engine/frame-stats.ts` explains
why that is honest rather than a stand-in. Consuming the boundary is #948's.

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

The rule is #944's: each entry arrives with the issue that uses it, and each
carries a line saying what replacing it would cost. `@xyflow/react`,
`react-arborist` and `@use-gesture/react` are still absent for that reason —
they belong to #950 and to the node canvas, and adding them early would mean
dependencies whose justifications nobody could check against real code.

| Package | What it does here | Replacing it |
|---|---|---|
| `react` / `react-dom` | The UI runtime. The substrate decision from #944, not a preference | A rewrite of the editor |
| `vite` | Dev server with fast refresh, and the production build | Weeks, and worse. This is the "not the place to be interesting" choice |
| `@vitejs/plugin-react` | JSX transform and fast refresh for Vite | Nothing else does this job for this bundler |
| `typescript` | Strict mode, and the build fails on a type error | The `.ts` in "the editor is a TypeScript application" |
| `vitest` | The component suite (#944, Q6) | A day to move to `node:test` + a DOM shim, and the shim is what Q6 rejected |
| `jsdom` | The DOM Vitest runs components against | Interchangeable with `happy-dom`; a config line |
| `@testing-library/react` + `@testing-library/dom` | Rendering and querying components in tests | Hand-rolled render helpers — a few hundred lines that drift |
| `react-resizable-panels` | The dock splitters: keyboard-resizable separators with the right ARIA, pointer capture that survives leaving the window, collapse/expand, and constraint solving across nested groups | A week of pointer maths and a permanent source of off-by-a-pixel bugs, for a control nobody will ever compliment |
| `@radix-ui/react-menubar` | The command bar: roving focus across the bar, arrow keys and type-ahead within a menu, Escape, focus restoration, `aria-expanded`, and submenus that flip rather than clip at the screen edge | A month, and a keyboard-accessible menu implementation to maintain forever. The menu *content* is entirely ours — Radix contributes behaviour, not vocabulary |
| `@radix-ui/react-tabs` | The dock tab strips and the inspector's tab group: the `tablist`/`tab`/`tabpanel` roles wired to each other, and arrow-key navigation | A hand-rolled tab strip, which is the one a screen reader cannot use |
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
| **Vitest** (`pnpm test`) | The layout model, the command table, the shell rendered, the boot path up to the module itself, the build plugin's path handling, and the stylesheet's own text | The `workspace` job — no emsdk, no browser |
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

- **Panel contents** — the outliner's tree (#950), the inspector's property grid
  (#951), the asset browser (#952). Each is a placeholder that says so on screen
- **The canvas handover** — camera and picking are #949. The viewport panel
  holds the canvas and keeps its id; it does not yet drive it
- **Driving the boundary** — `@kruddage/engine/bridge` exists as of #945, and
  nothing here calls it. Wiring the command bar to #947's command layer and the
  status strip to the boundary is #948
- **The node graph** — #944's Q5 puts it here, in React, as one canvas. It
  arrives after the panels
- **A phone layout** — ruled out of #944 explicitly. Container queries and
  mount-not-duplicate went in anyway, because they are cheap insurance rather
  than a commitment
