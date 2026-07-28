<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# @kruddage/kruddmake

The build language. Around 2,200 lines of Scheme that read every module's
`build.scm`, resolve them against `manifest.scm`, and render one `build.ninja`
that `ninja(1)` then builds — natively with `cc`, or to WASM with `emcc`. No
CMake, no emcmake, no second graph.

This is a package so that "only `@kruddage/engine` may drive the engine build"
is a dependency edge rather than a regex about a filename. It was previously
reachable only through a root-level shell script, a build entry point that
lived outside the build graph (#920).

## Surface

The operations, not the five `.scm` modules behind them:

```sh
krudd/kruddmake/kruddmake.sh              # resolve projects here: setup / run / pick
krudd/kruddmake/kruddmake.sh build        # configure + build (what CI runs)
krudd/kruddmake/kruddmake.sh run          # build, then serve the site
krudd/kruddmake/kruddmake.sh new-project  # scaffold a <name>.krudd-project
```

`kruddmake.sh` builds the `krudd` host tool if it is missing or stale and execs
it. From inside the workspace the same entry point is on `PATH` as `kruddmake`,
because this package declares it as a `bin`.

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
| `run-scheme-tests.sh` | this package's `test` |
| `run-tests.sh` | the whole native suite (see below) |

`../krudd.c` — 239 lines, the s7 front door `kruddmake.sh` compiles and execs —
belongs to this package too. It stays at `krudd/krudd.c` because a package does
not get to move sources to look tidier (#917), and because it sits beside
`third_party/s7.h`, which is the only header it includes.

## Tests

```sh
pnpm --filter @kruddage/kruddmake test    # or: sh krudd/kruddmake/run-scheme-tests.sh
```

`introspect_test.scm` and `resolve_test.scm` — the codegen helpers, the resolver
and the ninja emitter — run on the pinned `krudds7` interpreter that
`krudd/third_party/sync.sh` fetches. **No emsdk, no ninja, no C compiler.** That
is the point of the split: the build language is checkable without a toolchain,
and the toolchain stages are what the native suite adds on top.

The native suite is a superset and is still a shell script:

```sh
sh krudd/kruddmake/run-tests.sh
```

It runs this package's Scheme suite first, then the module oracles, then builds
and runs the native tests through the generated `build.ninja`, then the WASM
link when `emcc` is present — each stage skipped with a message when its tool is
absent.

**Node is not in that path.** `run-tests.sh` and `kruddmake.sh` are POSIX shell
and reach nothing outside `krudd/`; a contributor with a compiler and no node
installed builds and tests the engine exactly as before. That pnpm can also
invoke kruddmake — through `@kruddage/engine`'s declared dependency on this
package — is a second door, not the door.
