# KRUDD

[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL--2.0--or--later-blue.svg)](https://spdx.org/licenses/GPL-2.0-or-later.html)
[![CI](https://github.com/kruddage/engine/actions/workflows/ci.yml/badge.svg)](https://github.com/kruddage/engine/actions/workflows/ci.yml)

A game engine written in **Rust** and **TypeScript**, compiled to WebAssembly
and served as a static site. Web first, WebGL2 first, with native Windows and
Linux to follow via Tauri 2.

> **krudd 2 is a rewrite in progress.** The C and Scheme engine that used to
> live here has been deleted, not strangled — see
> [#812](https://github.com/kruddage/engine/issues/812) for the plan and
> [#814](https://github.com/kruddage/engine/issues/814) for what left and why.
> What is here now is the spine: a package boundary, a build driver, and a
> loadable wasm module. **It renders nothing yet.** The WebGL2 hello triangle
> is [#818](https://github.com/kruddage/engine/issues/818), the real HTML
> shell is [#819](https://github.com/kruddage/engine/issues/819), and the live
> demo and the Flatpak are dark until they land. That window is the accepted
> cost of delete-and-replace, not a regression.

## The shape of it

Rust owns the hot path; TypeScript owns everything else.

| | Rust (`crates/`) | TypeScript (`packages/`) |
|---|---|---|
| **Runs as** | one wasm module | one ES module bundle |
| **Memory** | no GC, manual lifetimes | the host engine's collector |
| **Owns** | render graph, GPU resources, math, physics, asset decode, ECS storage | editor, gameplay, scene authoring, tooling |

TypeScript is the GC half because the browser's collector is already there and
already paid for — in the browser and in a Tauri webview alike, so the engine
ships zero GC bytes of its own.

The boundary between them is **batched, never per-object**: TypeScript reads
typed-array views straight over wasm linear memory and calls into Rust once
per phase. Getting that wrong looks like "wasm is slow" and is not — reading a
world one call per entity measures ~36× more expensive than reading it through
the view, which is what `cargo xtask bench` exists to keep true.

See [`docs/architecture.md`](docs/architecture.md) for the tier order, what
each crate and package owns, and what each may reach for, and
[`docs/boundary.md`](docs/boundary.md) for the contract across the wasm
boundary — ownership, what may be exported, and when a view goes stale.

## Building

### Prerequisites

- **Rust** via [rustup](https://rustup.rs/) — `rust-toolchain.toml` pins the
  channel and installs the wasm target and components on first use, so there
  is nothing to add by hand.
- **Node 22+** and **pnpm 10** — `npm install -g pnpm`, or `corepack enable pnpm`.

`cargo xtask` installs the matching wasm-bindgen CLI itself the first time it
needs one.

### One entry point

```sh
cargo xtask build-web    # wasm + TypeScript into dist/
cargo xtask serve        # build, then serve dist/ at http://127.0.0.1:8080/
cargo xtask dist         # optimised build, with the artifact sizes
cargo xtask check        # everything CI gates on
cargo xtask test-web     # just the boundary tests, against the built wasm
cargo xtask bench        # the batched boundary against the per-call one
cargo xtask tiers        # just the crate/package tier check
```

There is no second build system and no step that exists only inside a CI
workflow: `ci.yml` runs `cargo xtask check`, so a green local run is a green
CI run. `cargo xtask help` lists the flags.

What you should see from `cargo xtask serve`: a dark page with **one line of
status text** — the version, the entity count, the frame number, the frame
rate, and the position of entity 0, which moves. That is Rust simulating and
TypeScript reading the result out of wasm memory with no copy. There is no
canvas yet; if you were expecting pixels, see #818.

## Layout

```
crates/            The Rust half, in tier order
  base/math/         Vectors, matrices, the spatial types
  world/storage/     Struct-of-arrays slots, generations, tombstones
  world/asset/       Asset decoding: bytes in, engine data out
  render/gpu/        Typed, generational GPU resource handles
  render/renderer/   The Backend trait and the Frame it is handed
  shell/web/         The wasm module the browser loads
packages/          The TypeScript half, same tier vocabulary
  base/boundary/     Loading the wasm and viewing its memory
    harness/           node:test over the memory contract, and the benchmark
  shell/web/         The browser page
xtask/             The build driver
docs/              Architecture and toolchain decisions
packaging/flatpak/ The old editor's Flatpak. Dark until #821 replaces it.
```

A crate or package may depend on its own tier or a lower one, never a higher
one, and nothing may depend on a shell. `cargo xtask tiers` enforces it — a
violation fails the build, not review.

## CI

| Workflow · job | What it does |
|---|---|
| **ci · check** | `cargo xtask check` — tiers, `rustfmt`, `clippy -D warnings`, `cargo test`, the wasm + TypeScript build, `tsc`, `biome ci`, and the boundary tests against the built module |
| **pr-title** | Checks the PR title is a valid Conventional Commit (it becomes the squashed commit) |
| **release-please** | On push to `main`, maintains the release PR that versions, tags, and releases |

`flatpak-build` is dispatch-only and cannot currently succeed — it packages the
deleted C editor. It is kept rather than removed because
[#821](https://github.com/kruddage/engine/issues/821) replaces its contents
rather than the workflow.

Publishing to GitHub Pages, and the per-PR preview deploys, are
[#820](https://github.com/kruddage/engine/issues/820) — there is nothing worth
looking at to publish until the triangle lands.

## Versioning and releases

Versioning is handled by [release-please](https://github.com/googleapis/release-please),
driven by [Conventional Commits](https://www.conventionalcommits.org/). We
squash-merge, so a PR's title *is* its commit message and the **pr-title**
check enforces the format:

- `feat: …` → minor bump &nbsp;·&nbsp; `fix:`/`perf: …` → patch bump &nbsp;·&nbsp; `feat!:` or a `BREAKING CHANGE:` footer → major bump
- `chore:`/`docs:`/`ci:`/`refactor:`/`test:`/`build: …` → no version bump, but still recorded in `CHANGELOG.md`

On each push to `main`, release-please opens or updates a single **release PR**
that rolls up the unreleased commits: it bumps [`version.txt`](version.txt),
regenerates `CHANGELOG.md`, and updates `.release-please-manifest.json`.
Merging that PR tags `vX.Y.Z` and cuts a GitHub Release.

`cargo xtask` reads `version.txt` and stamps it into the wasm and the page;
`KRUDD_VERSION` overrides it so a preview build can mark itself as one.

The version number carries across the rewrite unbroken — the engine is the
same project, not a new one. The Rust crates all sit at `0.0.0` and are
`publish = false`, deliberately: two version numbers would drift, and nothing
here goes to crates.io.

## License

GPL-2.0-or-later for open source and GPL-compliant use. Use in proprietary or
commercial products requires a separate commercial license from the author.

External contributions require a CLA (copyright assignment). Contact the
project maintainer for details.

## Contributing

See [`CODING_STANDARD.md`](CODING_STANDARD.md) before writing or reviewing
anything. The short version: `rustfmt` and `clippy` at their defaults, Biome at
its defaults, and when two forms are both correct the canonical one wins.

Run `cargo xtask check` before pushing. It is the same command CI runs.
