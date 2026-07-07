# Vendored: s7 Scheme (krudd's build interpreter)

`s7.c` / `s7.h` are vendored third-party source, **not** engine-authored code.
They keep their upstream `SPDX-License-Identifier: 0BSD` header and are **not**
stamped with the project's `GPL-2.0-or-later` line — that line marks our own
files; s7 keeps its own notice (see `LICENSE.s7`).

s7 is linked into the `krudd` **host tool** (see `../krudd.c`), where it runs the
Scheme build description `../build.scm`. This is s7 as *build authority* — the
same interpreter will later run in the browser as a side module, but that is a
separate deployment.

## Pin

| Field    | Value |
|----------|-------|
| Version  | s7 **10.8** |
| Date     | 17-Apr-2024 (upstream `S7_DATE`) |
| License  | 0BSD (Zero-Clause BSD) — permissive, zero conditions |
| Upstream | https://ccrma.stanford.edu/software/snd/snd/s7.html |
| Mirror   | https://raw.githubusercontent.com/aboalang/s7 (`s7.c`, `s7.h`) |

s7 is a single-file, rolling release identified by version + date rather than a
git tag. That pair is the pin; re-vendoring means replacing both files from the
same release and bumping this table.

## License compatibility

0BSD combining into a `GPL-2.0-or-later` work is the standard permissive-inbound
case, and 0BSD imposes *no* downstream conditions (not even attribution), so
there is nothing to reconcile. Because s7 is third-party it sits outside the
project's CLA / copyright-assignment surface, and 0BSD poses no obstacle to the
commercial license offered alongside the GPL build.

## Build-time feature configuration

Set in `../krudd.sh` when compiling the tool:

| Define            | Value | Rationale |
|-------------------|-------|-----------|
| `WITH_C_LOADER=0` | off   | Drops the `dlopen`-based external-C-library loader — unneeded and unwanted surface for a build tool. |
| `WITH_MAIN=0`     | off   | Keeps s7 a library linked into `krudd`, not a standalone REPL binary. |

s7 is compiled with `-w`: it is upstream code, so the engine's warning flags are
not enforced against it.
