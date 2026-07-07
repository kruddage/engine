# Scheme runtime (s7) — Phase 0

This documents the Phase-0 foundation from [#271](https://github.com/kruddage/engine/issues/271):
vendoring the [s7 Scheme](https://ccrma.stanford.edu/software/snd/snd/s7.html)
interpreter and getting it to **compile and evaluate** on both engine targets,
bound to nothing engine-aware yet. It is the toolchain spike for the broader
Scheme-substrate initiative ([#270](https://github.com/kruddage/engine/issues/270)).

## Why s7

s7 is a single-file, dependency-free, embeddable Scheme with first-class C
interop (Schottstaedt, CCRMA/Stanford). "Single file" (`s7.c` + `s7.h`) is what
makes it vendor cleanly into the existing per-plugin build idiom without a
sub-build or external package.

## License & CLA (resolves #276)

- **s7 license: 0BSD** (Zero-Clause BSD), declared by the
  `SPDX-License-Identifier: 0BSD` line in `s7.c` and `s7.h`. 0BSD is the most
  permissive license there is — use/copy/modify/distribute with no conditions,
  not even attribution retention.
- **Compatibility with `GPL-2.0-or-later`: yes, trivially.** A permissive
  inbound license combining into a GPL work is the standard, well-understood
  case; 0BSD imposes *zero* downstream conditions, so there is nothing to
  reconcile. The combined distribution remains `GPL-2.0-or-later`; the vendored
  files keep their own 0BSD notice.
- **CLA / commercial dual model:** vendored third-party code is **not**
  author-owned and sits **outside** the copyright-assignment surface. The CLA
  governs contributions to engine-authored code; it does not — and need not —
  cover s7. Because 0BSD permits relicensing/redistribution without conditions,
  s7 poses no obstacle to the commercial license offered alongside the GPL
  build.
- **Notice placement:** the upstream notice is preserved in-file, and a
  standalone copy lives at `modules/scheme/third_party/LICENSE.s7`, with the pin
  and feature configuration in `modules/scheme/third_party/VENDOR.md`.

Because s7 is 0BSD, its source files are **not** stamped with the project's
`GPL-2.0-or-later` SPDX line — that line marks engine-authored files. The
engine-authored wrapper (`scheme.c`, `scheme.h`, `scheme_test.c`) carries the
`GPL-2.0-or-later` line as usual.

## Build (resolves #278)

`modules/scheme/CMakeLists.txt` mirrors the established side-module idiom
(cf. `plugins/vscript`):

- **Native** (`NOT EMSCRIPTEN`): `s7` static lib + `scheme_plugin` static lib +
  a `scheme_test` executable registered with ctest.
- **WASM** (`EMSCRIPTEN`): an `add_custom_command` compiling `scheme.c` +
  `third_party/s7.c` together with `-sSIDE_MODULE=1 -O2` into `scheme.wasm`,
  hung off the `index` target via `add_dependencies`.

s7 is compiled with `-w` (it is upstream code; the engine's `-Wall -Werror
-Wpedantic` is not enforced against it) and the feature defines documented in
`VENDOR.md` (`WITH_C_LOADER=0`, `WITH_MAIN=0`).

### Allocation

s7 owns its heap internally through libc `malloc`/`free` (native) or the WASM
heap supplied by the main module (side module). Phase 0 does **not** route s7's
internal allocation through `memory_api`: s7 needs `realloc`, which the current
`memory_api` vtable does not expose, so a faithful shim means adding a
size-tracking `realloc` — deferred to Epic B, where the engine bindings land and
the vtable is revisited. The engine-authored wrapper allocates nothing of its
own (one static interpreter), so it introduces no raw allocation.

## Runtime integration (resolves #280)

`scheme.c` registers a `"scheme"` subsystem. On WASM the exported `plugin_entry`
is invoked by the plugin loader; `"scheme.wasm"` is listed early in the
`plugins[]` load order in `modules/core/engine.c` (it imports nothing from other
plugins, so it is a valid topological root). The subsystem's `init` hook boots
the interpreter and logs a live smoke eval (`(+ 1 2) = 3`); `shutdown` frees it.

## API surface (Phase 0)

`scheme.h` exposes a deliberately tiny `struct scheme_api` — enough to prove
evaluation, not to script the engine:

| Function | Contract |
|----------|----------|
| `eval_int(src, out)`        | integer result → `*out`, return 0; else −1 |
| `eval_string(src, buf, n)`  | string result copied into `buf`, return 0; else −1 |
| `eval_ok(src)`              | 0 if evaluated without a Scheme error, else −1 |

Errors are detected by redirecting s7's error port into a string port, so a
Scheme-level error is observable (non-empty capture) and never reaches stderr,
and the interpreter stays usable afterward. `scheme_test.c` exercises
arithmetic, strings, and error paths (division by zero, unbound variable, bad
`car`, unbalanced parens) under ctest.

## Explicitly out of scope

Engine bindings (Epic B), `.scm` assets (Epic C), visual-graph lowering
(Epic D), and Scheme-as-build-authority (Epic E). Phase 0 evaluates arithmetic
and strings — nothing engine-aware.
