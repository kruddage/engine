# Toolchain

Every choice here, and why. The rule behind all of them is the one in
[`CODING_STANDARD.md`](../CODING_STANDARD.md): this engine is built by many
hands and many agents, most of which never meet, so when two forms are both
correct the canonical one wins. That means taking each tool's defaults and
picking the option that costs the fewest arguments — not the option with the
most knobs.

## Rust

| | |
|---|---|
| **Toolchain** | `stable`, pinned in `rust-toolchain.toml` |
| **Edition** | 2024 |
| **Format** | `rustfmt`, defaults |
| **Lint** | `clippy`, defaults, `--deny warnings` in CI |
| **Target** | `wasm32-unknown-unknown` |

The channel is `stable` rather than a fixed version because the style gates
*are* rustfmt and clippy at their defaults, and those move with the toolchain
by design. Pinning would freeze them and make every upgrade a diff.

`rust-toolchain.toml` also lists the wasm target and the two components, so
rustup installs them on the first `cargo` invocation. That is what makes
"works from a clean checkout" true rather than aspirational — nobody has to
remember `rustup target add`.

Every crate turns on `missing_docs`. Exporting something then costs a doc
comment, which makes the public surface a decision rather than a keyword.

## wasm-bindgen

The Rust↔JavaScript glue. Chosen over hand-written `extern "C"` because it
generates the TypeScript declarations as well as the JavaScript, so the two
sides of the boundary cannot drift within a build.

Its CLI and the `wasm-bindgen` crate **must** be the same version — a mismatch
produces "invalid schema version" at page load rather than an error at build
time. `cargo xtask` reads the resolved version out of `Cargo.lock` and
installs the matching CLI if the one on `PATH` disagrees, so a `cargo update`
cannot desynchronise them and nobody has to know the rule.

The Component Model and WIT are **not** used. They exist to link two compiled
languages into one wasm binary; one Rust module plus a TypeScript host has no
such problem, and Cargo crate visibility is a cheaper API-leak defence than a
wasm interface layer.

## TypeScript

| | |
|---|---|
| **Runtime** | Node 22+ |
| **Package manager** | pnpm 10 |
| **Bundler** | esbuild |
| **Typechecker** | `tsc`, `noEmit`, every strict flag on |
| **Format + lint** | Biome, recommended preset, defaults |
| **Tests** | `node:test`, run over an esbuild bundle |

**Why TypeScript at all** is [#812]'s call: the host JavaScript engine's
collector is already present and already paid for, so the GC half of the
engine ships zero GC bytes. Go via
TinyGo was the alternative and was the weaker half — its wasip2 target
hardwires the `wasi:cli/command` world, so the custom WIT worlds it would have
been brought in for are not yet expressible.

**pnpm over npm/Yarn/Bun/Deno.** Workspaces without a plugin, a strict
`node_modules` layout that makes an undeclared dependency a resolution error
rather than a lucky hoist, and it is a package manager rather than a runtime —
so it composes with whatever Node the machine has instead of being another
thing to install and pin. Bun and Deno both bundle a runtime and a test runner
we would then have to decide whether to adopt; that is a bigger decision than
this stage needs, and it can still be made later, because nothing in the tree
depends on pnpm beyond the lockfile.

`pnpm install --frozen-lockfile` is what xtask runs, explicitly. It is pnpm's
default in CI and not outside it, and a local build that resolves a different
tree than CI is a class of bug worth spending one flag to remove.

**esbuild over Vite/Rollup/webpack.** The web half is one entry point that
produces one ES module, with no framework, no HMR requirement and no plugin
pipeline. esbuild does exactly that in milliseconds and is a single binary.
Vite is the obvious upgrade path if the editor ever wants dev-server niceties
([#819], [#830]) — and it runs esbuild underneath, so the choice is not a
dead end.

**Biome over ESLint + Prettier.** One tool where the ecosystem default is two,
on its recommended preset, at its default settings (tabs, 80 columns). Two
tools with overlapping opinions put back exactly the argument a style gate
exists to remove. Biome's `ci` mode checks formatting, lint and import order
in one pass and exits non-zero, which is all CI needs.

**TypeScript 5.9, not 7.** TypeScript 7 is the native port. Adopting it is a
real decision with real upside and it should be made on its merits, not
inherited by default at the moment the tree is being stood up. The pin is
recorded here so the next person knows it was noticed rather than missed.

**`node:test` over a bundle, and no test runner installed.** The prediction
below held: the first TypeScript worth testing was the boundary wrapper, and
Node's built-in runner covered it without adding a dependency. It runs over an
esbuild bundle rather than the sources because the harnesses import a package
by its `exports` map, and bundling is what resolves that —
`--experimental-strip-types` would run the files but not find their imports.
`cargo xtask test-web` does both steps; `cargo xtask check` runs it.

**One `tsconfig.json` for the whole workspace,** not one per package. Package
boundaries are enforced by each `package.json`'s `exports` map — which tsc and
esbuild both honour, so a deep import is a resolution error — and by
`cargo xtask tiers`. Splitting the config would add a place for two packages
to disagree about strictness without adding any enforcement.

The one exception is `tsconfig.harness.json`, and it splits on *environment*
rather than on package: the harnesses run under Node and need `@types/node`,
and browser code must not be able to see `process` or `node:fs`. Merging them
would put Node's globals in scope for the whole tree. Both configs typecheck
in `cargo xtask check`.

`noEmit` is on: esbuild emits, tsc only typechecks. Two things emitting the
same code is how the two come to disagree.

## The build driver

`cargo xtask`, a plain Rust binary aliased in `.cargo/config.toml`. It replaces
`kruddmake`, the old tree's S7 Scheme build language, and keeps the property
that made kruddmake worth having: the build driver is a first-party program in
the same language as the engine, so "krudd builds krudd" stays true with Rust
in the seat Scheme held.

**It has no dependencies.** An argument parser and a static file server are
each under a hundred lines of `std`, and xtask is the first thing a clean
checkout compiles — every crate in it is latency between `git clone` and a
running engine.

The dev server is hand-written for one concrete reason beyond that:
`application/wasm` is not optional. `WebAssembly.instantiateStreaming` rejects
any other content type, and the usual one-liners get it wrong on some
platforms, which surfaces as a page error rather than a server one. Owning the
MIME table is cheaper than documenting the workaround, and there is a test on
it.

## What is deliberately absent

- **WebGPU.** Deferred entirely ([#812]). WebGL2 runs in every browser the
  engine ships to, and the browser is now the only place it ships ([#845]), so
  a second backend buys nothing until there is something the first one cannot
  draw. It was a risk when the plan included a WebKitGTK-based native shell,
  which ships no `navigator.gpu`; dropping the native shells removed the risk
  rather than deferring it.
- **A test runner for TypeScript.** `node:test` is the runner, and it is not
  a dependency — see above. Vitest and friends buy watch mode, mocking and a
  browser environment; nothing here wants the first two, and the third is a
  browser harness ([#818], [#820]) rather than a runner choice.
- **A benchmark library.** `cargo xtask bench` times two loops with
  `performance.now()` and prints a table. A statistical harness would be worth
  it for a 5% regression; the thing being measured here is a 36× cliff.
- **A native build.** No Tauri, no Electron, no Flatpak, no Windows target
  ([#845]). A native shell is a second platform to keep the renderer, the
  packaging and the release working on, and the engine is not yet finished
  being right on one.
- **A browser automation library.** `cargo xtask render-test`'s
  screenshot-and-compare harness ([#827]) drives Chromium itself, over the
  DevTools Protocol, rather than through Puppeteer or Playwright: Node's
  built-in `WebSocket` is the whole transport, and the page it already is a
  PNG codec decodes the PNGs — the two things such a library would mostly be
  buying here. What it does not replace is the browser binary itself, which
  the harness expects to already be on disk (a few conventional paths, or
  `KRUDD_CHROMIUM`/`CHROME_PATH`); CI installs one with a GitHub Action, not
  a project dependency. See `packages/shell/web/harness/cdp.ts` and
  `chromium.ts`.

[#812]: https://github.com/kruddage/engine/issues/812
[#818]: https://github.com/kruddage/engine/issues/818
[#819]: https://github.com/kruddage/engine/issues/819
[#820]: https://github.com/kruddage/engine/issues/820
[#827]: https://github.com/kruddage/engine/issues/827
[#830]: https://github.com/kruddage/engine/issues/830
[#845]: https://github.com/kruddage/engine/issues/845
