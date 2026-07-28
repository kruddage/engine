<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# kruddmake

The build language. Around 2,200 lines of Scheme that read every module's
`build.scm`, resolve them against `manifest.scm`, and render one `build.ninja`
that `ninja(1)` then builds — natively with `cc`, or to WASM with `emcc`. No
CMake, no emcmake, no second graph.

**This is an entry point, not a package.** `kruddmake.sh` is the front door to
the C build, and the front door is a path: a contributor with a compiler runs it
in a fresh checkout with no Node installed and nothing above `krudd/` set up.
That is the property being protected, and it is why `krudd/` is not in the pnpm
workspace (#934, [`WORKSPACE.md`](../../WORKSPACE.md) Q2). It briefly carried a
`package.json` (#920); what that bought — "only `@kruddage/engine` may drive the
engine build", as a dependency edge — is enforced by path instead, in
`scripts/check-barriers.mjs` rule 3.

## Entry points

The operations, not the five `.scm` modules behind them:

```sh
krudd/kruddmake/kruddmake.sh              # resolve projects here: setup / run / pick
krudd/kruddmake/kruddmake.sh build        # configure + build (what CI runs)
krudd/kruddmake/kruddmake.sh run          # build, then serve the site
krudd/kruddmake/kruddmake.sh new-project  # scaffold a <name>.krudd-project
```

`kruddmake.sh` builds the `krudd` host tool if it is missing or stale and execs
it. There is no shorter name for it on `PATH`: it was briefly a workspace `bin`,
which put it on `PATH` only for someone who had already run `pnpm install`, and
that is the one contributor who needs it least.

There is no `clean` verb. Removing a build directory is `rm -rf` on whatever
`KRUDD_BUILD_DIR` pointed at, and the artifact side of it —
`packages/engine/dist` — belongs to `@kruddage/engine`, which has a `clean`
script for exactly that.

### Environment

The generator's interface, unchanged. The sanitizer and coverage CI jobs are
wired to it:

| | |
|---|---|
| `KRUDD_TARGET` | `wasm` to build the WASM module; anything else builds native |
| `KRUDD_BUILD_DIR` | where `build.ninja` and its objects land (default `build/`) |
| `KRUDD_VERSION` | the version stamped into the build (default `version.txt`) |
| `KRUDD_CC`, `KRUDD_CXX` | the compilers the generated build.ninja invokes |
| `KRUDD_EXTRA_CFLAGS`, `KRUDD_EXTRA_LDFLAGS` | flags appended to every edge — how the sanitizer and coverage builds are the ordinary build with instrumentation, rather than a fork of it |

## What is in here

| | |
|---|---|
| `manifest.scm` | the module directories in tier order — the source of truth for what a module may reach for |
| `build.scm` | the entry `krudd build` loads: read every spec, render, drive ninja |
| `resolve.scm` | specs → targets: link resolution, include paths, the tier check |
| `ninja.scm` | targets → `build.ninja` |
| `introspect.scm` | codegen: `embed`, `configure-file`, `emit-interface-header` |
| `kruddmake.sh` | the entry point above |
| `run-scheme-tests.sh` | the Scheme suite — no toolchain (see below) |
| `run-tests.sh` | the whole native suite (see below) |

`../krudd.c` — 239 lines, the s7 front door `kruddmake.sh` compiles and execs —
belongs here too. It stays at `krudd/krudd.c` because it sits beside
`third_party/s7.h`, which is the only header it includes.

## Tests

```sh
sh krudd/kruddmake/run-scheme-tests.sh
```

`introspect_test.scm` and `resolve_test.scm` — the codegen helpers, the resolver
and the ninja emitter — run on the pinned `krudds7` interpreter that
`krudd/third_party/sync.sh` fetches. **No emsdk, no ninja, no C compiler.** That
is the point of the split: the build language is checkable without a toolchain,
and the toolchain stages are what the native suite adds on top. CI's `lint` job
runs this script by path for exactly that reason — it has Node and no compiler,
and it used to reach these checks through `pnpm -r run test` (#934).

The native suite is a superset and is still a shell script:

```sh
sh krudd/kruddmake/run-tests.sh
```

It runs the Scheme suite first, then the module oracles, then builds and runs
the native tests through the generated `build.ninja`, then the WASM link when
`emcc` is present — each stage skipped with a message when its tool is absent.

**Node is not in that path.** `run-tests.sh` and `kruddmake.sh` are POSIX shell
and reach nothing outside `krudd/`; a contributor with a compiler and no node
installed builds and tests the engine exactly as before. That
`@kruddage/engine` can also invoke kruddmake — by this path, `pnpm --filter
@kruddage/engine run test:native` — is a second door, not the door.
