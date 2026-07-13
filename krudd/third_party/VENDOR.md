# stb_truetype (kruddgui's glyph baker) — vendored

`stb_truetype.h` is third-party, single-header, **public domain** source (its
upstream carries a public-domain dedication, kept verbatim — it is **not**
stamped with the project's `GPL-2.0-or-later` line). kruddgui compiles it into
the WASM module to bake its own glyph atlas from the UI font it fetches at
startup, replacing the one text coupling it had to Dear ImGui.

Exactly one translation unit (`../engine/ui/kruddgui/kgui_font.c`) defines
`STB_TRUETYPE_IMPLEMENTATION` before the include; every other user gets just the
declarations. Like s7, it is compiled with relaxed warnings — it is upstream
code, not held to the engine's `-Werror -Wpedantic`.

> **Trust boundary.** stb_truetype's own header warns it does no bounds checking
> and must not be run on untrusted font files. kruddgui only ever feeds it the
> first-party UI font it ships and fetches from its **own origin** (`ui_font.ttf`,
> as trusted as the module itself), never a user- or third-party file, so that
> warning does not apply to this use.

## Pin (stb_truetype)

| Field    | Value |
|----------|-------|
| Version  | stb_truetype **v1.26** |
| License  | Public domain (Unlicense alternative in-header) |
| Upstream | https://github.com/nothings/stb |
| Mirror   | pinned to commit `f0569113c93ad095470c54bf34a17b36646bbbb5` |

Re-vendoring means re-fetching `stb_truetype.h` at a new pinned commit and
updating the version/commit above.

---

# Inter (kruddgui's UI font) — vendored

`../engine/ui/kruddgui/assets/ui_font.ttf` is **Inter** (`InterVariable.ttf`),
the UI font kruddgui bakes its glyph atlas from at build time. It is
third-party, **not** engine-authored, and keeps its own SIL Open Font License
1.1 (`../engine/ui/kruddgui/assets/OFL.txt`) — it is **not** stamped with the
project's `GPL-2.0-or-later` line. OFL bundling into a GPL work is the standard
permissive-inbound case: the font is data, not linked code, so there is nothing
to reconcile, and OFL poses no obstacle to the commercial build.

It is served **unmodified** (fetched and rasterised at runtime, never edited or
renamed), so the OFL's Reserved-Font-Name and Modified-Version clauses do not
apply. See `../engine/ui/kruddgui/assets/README.md` for the baking notes
(kerning is GPOS-only in Inter, so inert through stb_truetype; the file is the
full multi-script face and a Latin subset would shrink the download).

## Pin (Inter)

| Field    | Value |
|----------|-------|
| Version  | Inter **4.001** (`InterVariable.ttf`) |
| License  | SIL Open Font License 1.1 |
| Copyright| Copyright 2016 The Inter Project Authors |
| Upstream | https://rsms.me/inter/ |
| Source   | https://rsms.me/inter/font-files/InterVariable.ttf |

Re-vendoring means replacing `ui_font.ttf`, refreshing `OFL.txt`, and bumping
the version above.

---

# s7 Scheme (krudd's build interpreter) — vendored

`s7.c` / `s7.h` are third-party source, **not** engine-authored code, and are
committed to this repo at the pinned commit in `s7.artifact`. `sync.sh` checks
the sha256 of the committed files before any build compiles them, so every
build compiles the exact same bytes; it can also (re-)download them from the
pinned commit when re-vendoring (see Pin below). They keep their upstream
`SPDX-License-Identifier: 0BSD` header and are **not** stamped with the
project's `GPL-2.0-or-later` line — that line marks our own files; s7 keeps
its own notice (see `LICENSE.s7`).

s7 is linked into the `krudd` **host tool** (see `../krudd.c`), where it runs the
Scheme build description `../build.scm`. This is s7 as *build authority* — the
same interpreter also runs in the browser, compiled straight into the single
WASM module through the `script` library, but that is a separate deployment.

## Fetch

`../krudd.sh` and both `run-tests.sh` harnesses source `sync.sh` before
compiling anything that touches s7 — including the `krudd.sh` bootstrap
itself, which needs `s7.c` before the `krudd` host tool (and its own
`krudd-fetch`) exist. `sync.sh` is idempotent: since the committed
`s7.c`/`s7.h` already match their pinned checksum, no network I/O happens in
the common case — fetching only kicks in if the committed files are missing
or a re-vendor bumped the pin.

## Pin

| Field    | Value |
|----------|-------|
| Version  | s7 **10.8** |
| Date     | 17-Apr-2024 (upstream `S7_DATE`) |
| License  | 0BSD (Zero-Clause BSD) — permissive, zero conditions |
| Upstream | https://ccrma.stanford.edu/software/snd/snd/s7.html |
| Mirror   | https://raw.githubusercontent.com/aboalang/s7, pinned by commit (see `s7.artifact`) |

s7 is a single-file, rolling release identified by version + date rather than a
git tag, so the pin is a mirror commit whose `s7.c`/`s7.h` match that
version+date exactly. Re-vendoring means bumping `s7.artifact` (version, date,
commit, checksums) — see the comment at the top of that file.

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
