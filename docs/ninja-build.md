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

## Not yet

The remaining #338 sub-issues build on this: de-verbatim the root spec (#341),
direct `emcc`/`em++` invocation replacing `emcmake` (#342), and switching
`krudd/build.scm` to drive Ninja and retiring the CMake backend (#343). Those
need the WASM toolchain to prove artifact equivalence and are out of scope here.
