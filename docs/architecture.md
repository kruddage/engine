# Architecture

krudd is one Rust wasm module and one TypeScript bundle, served as a static
site. Rust owns the hot path; TypeScript owns everything else. This document
is the layout: what the tiers are, what each crate and package owns, and what
each is allowed to reach for.

The rules here are not advisory. `cargo xtask tiers` walks both trees and
fails the build on a violation — see [The tier check](#the-tier-check).

## The split

| | Rust (`crates/`) | TypeScript (`packages/`) |
|---|---|---|
| **Runs as** | one wasm module | one ES module bundle |
| **Memory** | no GC, manual lifetimes | the host engine's collector |
| **Owns** | render graph, GPU resources, math, physics, asset decode, ECS storage | editor, gameplay, scene authoring, tooling |
| **Built by** | `cargo`, driven by `cargo xtask` | `esbuild`, driven by `cargo xtask` |

TypeScript is the GC half because the browser's collector is already present
and already paid for, in the browser and in a Tauri webview alike — zero GC
bytes shipped. It relocates GC pauses rather than removing them, which is the
same deal Unity and Godot live with.

### The boundary is batched, never per-object

TypeScript reads and writes typed-array views directly over wasm linear memory
and calls into Rust **once per phase**. `World.tick()` advances everything;
`World.positions()` hands back a `Float32Array` mapped over Rust's own column,
not a copy.

A design that called across the boundary once per entity would be slow, and it
would present as "wasm is slow" rather than as the mistake it is. That is why
the storage is struct-of-arrays (`crates/world/storage`): a column per field
means the view *is* the memory, with no stride and no marshalling.

How much slower is measured rather than asserted — `cargo xtask bench` puts
the two paths side by side, and the per-call one reads ~36× more expensively.
[`boundary.md`](boundary.md) is the full contract: ownership, what may be
exported, and the three ways a typed-array view goes stale.

The boundary is hand-written on both sides today. Generating both halves from
one spec so they cannot drift is [#824].

## The tiers

Both trees use the same ordered tier vocabulary, and the tier is the directory
a crate or package sits in — so the ordering reads off the layout without
consulting a list. **A unit may depend on its own tier or a lower one, never a
higher one.**

| # | Tier | What it is for |
|---|---|---|
| 1 | `base` | No engine concepts at all: arithmetic, logging, allocation, and the wasm boundary itself. The spatial types live here because they are geometry rather than world data — which is what lets `base` stay strictly below `world`. |
| 2 | `world` | The scene and its data model: entity storage, assets, editing. |
| 3 | `render` | GPU resources, the renderer interface, and the passes that drive it. |
| 4 | `audio` | The mixer and its device backends. *(empty)* |
| 5 | `ui` | Editor chrome. HTML, per [#812] — the editor GUI is not the engine's business to draw. *(empty)* |
| 6 | `game` | Game code and the launcher registry. *(empty)* |
| 7 | `shell` | The hosts the engine runs inside. **Last on purpose: a shell may reach for anything, and nothing may reach for a shell** — not even another shell, since the web page and the Tauri window ([#821]) are alternative hosts rather than a layer. |

The empty tiers are listed anyway. The old tree's mistake was that the
ordering lived in people's heads until someone finally wrote it down; naming
the slot before there is anything in it costs a table row and settles an
argument in advance.

`abi` and `core`, the first two tiers of the old `kruddmake/manifest.scm`, are
gone rather than empty. `abi` held the plugin vtables, and `core` the
subsystem manager that dispatched through them — runtime service discovery by
string name, which existed because every module compiled into one blob with no
linker-level dependency graph. Cargo and TypeScript imports *are* that graph,
checked at compile time.

## The crates

```
crates/
  base/
    math/        krudd-math    Vectors, matrices, the spatial types
  world/
    storage/     krudd-world   Struct-of-arrays slots, generations, tombstones
    asset/       krudd-asset   Asset decoding: bytes in, engine data out
  render/
    gpu/         krudd-gpu     Typed, generational GPU resource handles
    renderer/    krudd-render  The Backend trait and the Frame it is handed
  shell/
    web/         krudd-web     The wasm module the browser loads
xtask/           xtask         The build driver — outside the tier order
```

| Crate | Owns | May reach for |
|---|---|---|
| `krudd-math` | `Vec2`, `Vec3`, `Mat4`. `#[repr(C)]` and column-major, because these are written into buffers that both the GPU and TypeScript read. | nothing |
| `krudd-world` | The slot table: generational handles, tombstones, and the rule that columns are sized to `capacity()` rather than `len()`. Owns no component data — columns are separate `Vec`s. | `base` |
| `krudd-asset` | The `Decode` trait, `DecodeError`, and `Reader` — the bounds-checked cursor every codec reads through, so the no-panic-on-hostile-input rule has one enforcement point. | `base` |
| `krudd-gpu` | `Id<K>`: a typed, generational handle. A handle crosses a language boundary where a `WebGLTexture` cannot, and it can outlive its resource safely. | `base`, `world` |
| `krudd-render` | `Viewport`, `Frame`, `Draw`, and the `Backend` trait — including the rule that `end_frame` is not `submit`. Holds no backend. | `base`, `world`, `render` |
| `krudd-web` | The `#[wasm_bindgen]` surface. **The only crate that knows wasm-bindgen exists**, which is what keeps every crate below it host-compilable and `cargo test` a real test run rather than a browser harness. | anything |
| `xtask` | The build. Not in the tier order and depends on no engine crate — it builds them, it is not built with them. | nothing |

Public surface is `pub`; everything else is private. Every crate sets
`missing_docs = "warn"`, so exporting something is a decision that costs a doc
comment rather than a keyword.

## The packages

```
packages/
  base/
    boundary/    @krudd/boundary    Loading the wasm and viewing its memory
      harness/                        node:test over the memory contract, and the benchmark
  shell/
    web/         @krudd/shell-web   The browser page
```

| Package | Exports | May be imported by |
|---|---|---|
| `@krudd/boundary` | `boot`, `World`, `BootOptions`, `Krudd` — via `"exports": { ".": "./src/index.ts" }`. Nothing else is reachable: a deep import is a resolution error, not a lint. | anything |
| `@krudd/shell-web` | nothing. Its `exports` map is empty, so importing it fails to resolve. | nothing |

`@krudd/boundary` is also the only module that imports wasm-bindgen's
generated glue. Everything above it goes through the wrapper, so there is one
place that knows how the module is loaded and one place that knows when a
typed-array view has gone stale — which matters, because growing wasm memory
*detaches* the old `ArrayBuffer` and every view over it silently becomes
zero-length. Its `harness/` directory holds that rule against the real module
under Node and times the alternative; see [`boundary.md`](boundary.md).

## The tier check

`cargo xtask tiers` (also run by `cargo xtask check`, which is what CI runs)
enforces four things across both trees:

1. **No upward dependency.** `base` cannot reach for `world`.
2. **Nothing depends on a shell**, including another shell.
3. **Every crate on disk is a declared workspace member.** A crate missing
   from `members` still compiles when something depends on it, and is silently
   skipped by `cargo test --workspace` otherwise.
4. **No stale members**, pointing at directories that no longer exist.

It reads the manifests with a deliberately naive line scanner rather than a
TOML/JSON parser — it only ever reads first-party manifests, and xtask having
no dependencies is worth more than generality. See `xtask/src/tiers.rs`.

## The build

One entry point, `cargo xtask`. There is no second build system, and no step
that exists only inside a CI workflow.

```
cargo xtask build-web    wasm + TypeScript into dist/
cargo xtask dist         the same, optimised, with artifact sizes
cargo xtask serve        build, then serve dist/ on :8080
cargo xtask check        fmt, clippy, tests, tiers, typecheck, lint, boundary tests
cargo xtask test-web     just the boundary tests, under Node
cargo xtask bench        the batched boundary against the per-call one
cargo xtask tiers        just the tier check
```

`build-web` runs in a fixed order, and the order is the point:

1. `cargo build -p krudd-web --target wasm32-unknown-unknown`
2. `wasm-bindgen` over the result → `packages/base/boundary/generated/`
3. `pnpm install` if `node_modules` is missing
4. `esbuild` bundles `packages/shell/web/src/index.ts` → `dist/index.js`
5. copy the wasm and stamp the version into `dist/index.html`

Step 2 generates the glue **and its `.d.ts`**, and step 4 compiles TypeScript
that imports them. Building TS without regenerating first would typecheck
against a stale boundary — the same "generated from without being rebuilt for"
failure the old tree's per-module `(embed …)` declarations existed to prevent.

xtask keeps the wasm-bindgen CLI in step with the `wasm-bindgen` crate by
reading the resolved version out of `Cargo.lock` and installing it if the one
on `PATH` disagrees. A mismatch there produces a confusing "invalid schema
version" at runtime rather than an error at build time.

See [`toolchain.md`](toolchain.md) for why each tool was chosen.

[#812]: https://github.com/kruddage/engine/issues/812
[#821]: https://github.com/kruddage/engine/issues/821
[#824]: https://github.com/kruddage/engine/issues/824
