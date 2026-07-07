# Vendored: s7 Scheme

`s7.c` and `s7.h` are vendored third-party source, **not** engine-authored code.
They keep their upstream `SPDX-License-Identifier: 0BSD` header and are **not**
stamped with the project's `GPL-2.0-or-later` line (see `docs/scheme-runtime.md`
for the license/CLA rationale).

## Pin

| Field    | Value |
|----------|-------|
| Version  | s7 **10.8** |
| Date     | 17-Apr-2024 (upstream `S7_DATE`) |
| License  | 0BSD (Zero-Clause BSD) |
| Upstream | https://ccrma.stanford.edu/software/snd/snd/s7.html |
| Mirror   | https://raw.githubusercontent.com/aboalang/s7 (`s7.c`, `s7.h`) |

s7 is a single-file, rolling release identified by version + date rather than a
git tag (upstream lives on ccrma / cm-gitlab, not GitHub). The version/date pair
above is the pin; re-vendoring means replacing both files from the same release
and bumping this table.

## Build-time feature configuration

Set in `modules/scheme/CMakeLists.txt` for both the native lib and the WASM side
module:

| Define            | Value | Rationale |
|-------------------|-------|-----------|
| `WITH_C_LOADER=0` | off   | Drops the `dlopen`-based loader for external C libraries — meaningless in a WASM side module and unwanted native attack surface for a spike. |
| `WITH_MAIN=0`     | off   | Keeps s7 a library; we embed it, we don't ship its standalone REPL binary. |

Bignums (`WITH_GMP`), the notcurses REPL, and the system-complex paths are left
at their upstream-default **off**, keeping the WASM footprint small. s7 is
compiled with `-w`: it is upstream code, so the engine's `-Wall -Werror
-Wpedantic` is not enforced against it.
