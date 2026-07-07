# Ninja build backend

Status: **incremental** — landed side-by-side with the CMake backend, not yet
wired into `./krudd.sh build`. Tracks initiative #338 (replace CMake with
krudd's own build system); this document covers sub-issues #339 (the emitter)
and #340 (the resolver).

## Why

krudd already owns the build description as Scheme data: every owned directory
carries a `CMakeLists.scm` — a backend-agnostic list of target forms
(`library`, `interface-library`, `executable`, `test`, `side-module`). Today
`krudd/cmake/cmake.scm` renders those forms to a `CMakeLists.txt` tree and lets
CMake drive Ninja. The end state of #338 is to render a `build.ninja` directly
and drive `ninja(1)` ourselves, keeping s7 as the one language that spans the
whole build.

This backend is the first two organs of that migration. It renders a real,
buildable `build.ninja` from the existing specs and is exercised on its own —
the CMake path still drives production builds until the Ninja path is proven
equivalent across native **and** WASM.

## Pieces

```
krudd/cmake/manifest.scm     the owned-directory list (shared source of truth,
                             read by both the CMake and Ninja backends)
krudd/ninja/resolve.scm      the include/link resolver (#340)
krudd/ninja/ninja.scm        the build.ninja emitter (#339)
krudd/ninja/resolve_test.scm native s7 checks + build.ninja generator
krudd/ninja/run-tests.sh     harness: run the checks, then build the native
                             suite through the generated build.ninja
```

### resolve.scm — the include/link resolver (#340)

CMake gives us one thing implicitly that a raw Ninja file cannot:
`target_link_libraries(A PRIVATE B)` hands A every PUBLIC include directory B
declares, transitively (A links B, B links C ⇒ A sees C's public headers).
Ninja has no targets or usage requirements, so this pass computes it explicitly.

Given the whole manifest as `(dir . spec)` pairs it builds a target table, then
per target resolves:

- **include directories** — the target's own PUBLIC then PRIVATE dirs, then
  every transitively linked library's PUBLIC dirs, de-duplicated. Paths come out
  relative to the tree root (`krudd/cmake/`).
- **link libraries** — the transitive closure of internal libraries it links,
  ordered dependents-first so a single-pass static link resolves.
- **system libraries** — `-l` flags (`m`), gathered across the whole closure so
  a static library's own dependency (asset_plugin links `m`) reaches the final
  executable, as CMake propagates a PRIVATE link requirement through the archive.

Two failure modes fail loudly at generation time rather than mis-linking later:
a **cycle** in the link graph, and a link entry that names neither a defined
target nor a known system library (a typo'd internal name).

The transitive include set the resolver computes matches — as a set, per target
— exactly what the current CMake build resolves for every native target
(`resolve_test.scm` asserts a sample against CMake's ground truth; the full
native build below confirms the rest end-to-end).

### ninja.scm — the emitter (#339)

Mirrors `cmake.scm`'s shape (same directory-spec forms) but emits Ninja
`rule`/`build` stanzas. CMake renders one file per directory; Ninja has no
directory model, so the emitter renders the **whole manifest** into a single
`build.ninja` and leans on `resolve.scm` for the include/link flattening.

| form                | output |
|---------------------|--------|
| `library`           | compile each source → object, archive to `lib<name>.a` |
| `interface-library` | no build output — include dirs only, via the resolver |
| `executable`        | compile sources, link against the transitive lib closure + system libs → `bin/<name>` |
| `test`              | a `run_test` stamp that runs the test executable |
| `side-module`       | one `emcc`/`em++` invocation → `<name>.wasm` (the WASM path) |

Objects live at `obj/<target>/<source>.o`, namespaced per target because the
same source compiled into two targets sees different include flags — exactly as
CMake gives each target its own `CMakeFiles/<target>.dir`.

The default target, `native`, groups every static library and every test stamp:
a bare `ninja` builds the whole native suite and needs no Emscripten toolchain.
Side modules are emitted (for form coverage) but build only when named
explicitly, since they need `emcc`.

## Verifying

```sh
./krudd/ninja/run-tests.sh
```

Stage 1 builds a native s7 interpreter and runs `resolve_test.scm` — the
resolver/emitter checks (include sets vs CMake, transitive closures, cycle and
unknown-target detection, emitted-stanza smoke). Needs only a C compiler.

Stage 2 (when `ninja` is present) renders the real manifest to `build.ninja` and
builds the `native` target. Each test links and runs as a `run_test` stamp, so a
green `ninja native` means the full native test suite passed through the
generated build — the same 29 tests, identically named, that CMake's `ctest`
runs.

## Root bootstrap (#341)

The root spec's imperative CMake bootstrap — reading VERSION, running git for the
build number and commit hash, and FetchContent-cloning imgui — used to ride
through `(verbatim ...)`. CMake did that work at configure time; the Ninja
backend has no `execute_process` or `FetchContent`, so krudd now owns it.
`krudd/introspect.scm` reads VERSION, derives the build number / commit hash from
git, and clones a pinned dependency, and `cmake.scm` gained the forms
(`cmake-minimum`, `repo-root`, `project`, `git-build-info`, `fetch-content`) that
bake those results in at synthesis time. The root spec is now free of imperative
CMake, so a future Ninja root renderer can lean on the same introspection.
`krudd/run-tests.sh` covers it (native s7 only).

## WASM build (#342)

The Ninja backend builds the whole WASM output — the main module
(`index.html`/`.js`/`.wasm`) and all 14 side modules — through explicit
`emcc`/`em++` calls, with no `emcmake`. What `emcmake`'s toolchain file supplied
implicitly (compiler, sysroot, target) `emcc` self-configures; the
Emscripten-specific flags (`-sMAIN_MODULE=1`, `-sMALLOC=mimalloc`,
`-sSIDE_MODULE=1`, …) are captured explicitly in the emitter's rules
(`main_module`, `side_module`).

The main module links WASM-compiled copies of the same libraries the native
tests use, so those are archived a second time with `emcc`/`emar` under `wasm/`
(separate from the native `obj/`, `lib*.a`). The `configure_file` outputs
(`version.h`, `shell.html`) and the changelog embed (`changelog_data.h`) are
generated by `krudd/introspect.scm` at synthesis time — krudd's replacements for
CMake's `configure_file` and `embed_changelog.cmake` — into `<builddir>/generated`.
imgui is resolved to the `#341` fetch checkout. A `wasm` target groups it all;
`ninja wasm` (with the Emscripten toolchain on `PATH`) builds it.

Verified against the CMake reference: of the 17 artifacts, 15 are **byte-identical**
(all 14 side modules and `index.html`) and 2 (`index.wasm`, `index.js`) are the
same size with **identical symbol tables** — the only byte differences are
emscripten's export/function ordering, which is order-insensitive for `wasm-ld`.
`krudd/ninja/run-tests.sh` builds and checks this when `emcc` is present.

## Not yet

The last #338 sub-issue: switch `krudd/build.scm` to drive Ninja and retire the
CMake backend (#343) — wiring the generated `build.ninja` into `./krudd.sh build`
for both native and WASM, and updating CI. Out of scope here.
