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
itself, which needs `s7.c` before the `krudd` host tool exists. `sync.sh`
is idempotent: since the committed
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

## Local patches

`s7.c` is **no longer byte-identical to the pinned upstream commit.** We carry
one local patch; `S7_C_SHA256` in `s7.artifact` is therefore the checksum of the
*patched* file, not of the upstream download.

| Patch | Where | Why |
|---|---|---|
| Drop the `(string-ref var 0)` → `string_ref_p_p0` swap in the `p_pi` branch of the fx optimizer | `s7.c`, search `KRUDD-LOCAL PATCH` | Upstream stores an `s7_p_pp_t` and then calls it through `s7_p_pi_t`. Benign UB on a register ABI; a hard `indirect call signature mismatch` trap on wasm, which killed the frame loop on any `(string-ref name 0)`. |

**Re-vendoring now has an extra step, and getting it wrong fails silently.**
`sync.sh` re-downloads whenever the committed file doesn't match `S7_C_SHA256`,
so a bump that only updates the checksums will quietly replace the patched
`s7.c` with pristine upstream and reintroduce the trap — with a green native
suite, because nothing native can see it. To re-vendor:

1. Check whether upstream fixed it (the `p_pi` branch near `fx_c_si_direct`). If
   so, drop the patch and this section, and take the plain upstream file.
2. If not, download upstream, re-apply the patch, *then* set `S7_C_SHA256` from
   the patched file.
3. Either way, verify in a **browser** — load a game and tap the board. The
   native suite cannot catch a wasm-only signature mismatch.

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

---

# stb_truetype (kruddgui's glyph baker) — vendored

`stb_truetype.h` is Sean Barrett's single-file TrueType rasterizer, third-party
source and **not** engine-authored code. It keeps its own upstream notice (public
domain / MIT dual license — see `LICENSE.stb_truetype`) and is **not** stamped
with the project's `GPL-2.0-or-later` line.

kruddgui's `kgui_font.c` includes it with `STB_TRUETYPE_IMPLEMENTATION` and
`STBTT_STATIC` (so every symbol is file-local, no link surface) to bake the
embedded JetBrains Mono face into a signed-distance-field atlas at load. It is
pure computation — no GL — so the same bake runs on the host under
`kgui_font_test`.

## Pin

| Field    | Value |
|----------|-------|
| Version  | stb_truetype **v1.26** |
| License  | Public domain (Unlicense) **or** MIT — dual, take your pick |
| Upstream | https://github.com/nothings/stb (`stb_truetype.h`) |
| sha256   | `ecd30b05e0dd4fea3a13c26810dd9e1992dc379049482c393d5a19e6b5090aab` |

Re-vendoring means replacing `stb_truetype.h` and updating the checksum above.

## License compatibility

Public-domain / MIT combining into a `GPL-2.0-or-later` work is the standard
permissive-inbound case; neither option imposes conditions that conflict, and
being third-party it sits outside the project's CLA surface.

---

# JetBrains Mono (kruddgui's embedded face) — vendored

The engine ships a single embedded typeface for kruddgui's text: **JetBrains
Mono Regular**, subset to printable ASCII (U+0020..U+007E) and emitted as the
byte array in `../engine/ui/kruddgui/jetbrains_mono.h`. It is font data, **not**
engine-authored code, and keeps its upstream **SIL Open Font License 1.1** (see
`LICENSE.jetbrains_mono`); the header carries an `SPDX-License-Identifier:
OFL-1.1` line rather than the project's `GPL-2.0-or-later`.

`kgui_font.c` bakes these bytes into the SDF atlas via the vendored
`stb_truetype.h`; there is no runtime file I/O and no GL dependency.

## Pin

| Field       | Value |
|-------------|-------|
| Face        | JetBrains Mono **Regular** |
| Coverage    | printable ASCII, U+0020..U+007E |
| License     | SIL Open Font License 1.1 |
| Upstream    | https://github.com/JetBrains/JetBrainsMono |
| Full sha256 | `e6fd0d7e91550b3ed2b735d4312474362c4716edc4fc0577a0f61ed782d5aed1` (upstream `fonts/ttf/JetBrainsMono-Regular.ttf`) |
| Subset sha256 | `25fc61bbeb720aa28ceaeb07e60f990c9e32e4457775dc00fe34dbff0d183e15` |

Re-vendoring (new weight or wider coverage) means re-running the `pyftsubset`
command recorded at the top of `jetbrains_mono.h`, re-emitting the array, and
updating the checksums above.

## License compatibility

OFL-1.1 permits bundling and redistribution inside another work (including a
GPL one); the font stays under OFL. The only conditions are that the font not be
sold on its own and that the reserved name "JetBrains Mono" not be used for a
modified version — neither of which the embedded, unmodified subset trips.
