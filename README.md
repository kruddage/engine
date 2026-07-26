# KRUDD

[![License: GPL-2.0-or-later](https://img.shields.io/badge/License-GPL--2.0--or--later-blue.svg)](https://spdx.org/licenses/GPL-2.0-or-later.html)
[![CI](https://github.com/kruddage/engine/actions/workflows/ci.yml/badge.svg)](https://github.com/kruddage/engine/actions/workflows/ci.yml)

A game engine written in **Rust** and **TypeScript**, compiled to WebAssembly
and served as a static site. Web only, WebGL2 only — the browser is the
platform, and there is no native build to keep in step with it.

> **krudd 2 is a rewrite in progress.** The C and Scheme engine that used to
> live here has been deleted, not strangled — see
> [#812](https://github.com/kruddage/engine/issues/812) for the plan and
> [#814](https://github.com/kruddage/engine/issues/814) for what left and why.
> What is here now is the spine: a package boundary, a build driver, a
> loadable wasm module, and **a triangle on screen** —
> [#818](https://github.com/kruddage/engine/issues/818),
> [#819](https://github.com/kruddage/engine/issues/819) and
> [#820](https://github.com/kruddage/engine/issues/820), which puts the live
> demo back up. The native shells that used to be on the plan are gone rather
> than pending — see
> [#845](https://github.com/kruddage/engine/issues/845).

## The shape of it

Rust owns the hot path; TypeScript owns everything else.

| | Rust (`crates/`) | TypeScript (`packages/`) |
|---|---|---|
| **Runs as** | one wasm module | one ES module bundle |
| **Memory** | no GC, manual lifetimes | the host engine's collector |
| **Owns** | render graph, GPU resources, math, physics, asset decode, ECS storage | editor, gameplay, scene authoring, tooling |

TypeScript is the GC half because the browser's collector is already there and
already paid for, so the engine ships zero GC bytes of its own.

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

What you should see from `cargo xtask serve`: a dark page with **eight
coloured triangles** drifting outward from the centre, and a line of readout
along the bottom — the version, what the renderer actually picked, the drawing
buffer size, the frame rate, and the draw count. Rust simulates and draws;
TypeScript reads the world out of wasm memory with no copy and writes back into
the same view to recycle the ones that drift off screen.

If something is wrong you get a full-screen message instead, not a blank
canvas. That is deliberate: a renderer that stops drawing leaves its last frame
up, so a failure has to be louder than the picture it is hiding behind.

## Layout

```
crates/            The Rust half, in tier order
  base/math/         Vectors, matrices, the spatial types
  world/storage/     Struct-of-arrays slots, generations, tombstones
  world/asset/       Asset decoding: bytes in, engine data out
  render/gpu/        Typed, generational GPU resource handles
  render/renderer/   The Backend trait and the Frame it is handed
  render/webgl/      The WebGL2 backend, on wgpu
  shell/web/         The wasm module the browser loads
packages/          The TypeScript half, same tier vocabulary
  base/boundary/     Loading the wasm and viewing its memory
    harness/           node:test over the memory contract, and the benchmark
  shell/web/         The browser page
xtask/             The build driver
docs/              Architecture and toolchain decisions
```

A crate or package may depend on its own tier or a lower one, never a higher
one, and nothing may depend on a shell. `cargo xtask tiers` enforces it — a
violation fails the build, not review.

## CI

| Workflow · job | What it does |
|---|---|
| **ci · check** | `cargo xtask check` — tiers, `rustfmt`, `clippy -D warnings`, `cargo test`, the wasm + TypeScript build, `tsc`, `biome ci`, and the boundary tests against the built module |
| **ci · deploy** | On push to `main`, publishes the release build to the live site |
| **ci · preview** | Publishes each pull request to its own URL, and takes it down when the PR closes |
| **pr-title** | Checks the PR title is a valid Conventional Commit (it becomes the squashed commit) |
| **release-please** | On push to `main`, maintains the release PR that versions, tags, and releases |

`check` is the only one of these that gates a merge. `deploy` and `preview`
publish; a failed preview does not hold up an auto-merge.

Both publish to the **`gh-pages` branch**, not through the Pages "GitHub
Actions" source: the site and every open PR's preview live on the one branch,
so they are serialised under a shared concurrency group and publish with
`keep_files` rather than mirroring. A preview build stamps its version
`-pr<N>+<sha>`, so the version the page prints tells you which build you are
looking at.

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
