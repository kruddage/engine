# s7 Scheme (krudd's build interpreter) — latest release artifacts

s7 is third-party, **not** engine-authored code. It is no longer vendored as
source in this repo. Instead it always builds against the *latest* release of
**`kruddage/s7`** — the org's own repo that re-cuts upstream [S7 Scheme][upstream]
as tagged GitHub Releases and ships prebuilt, linkable artifacts. `s7.artifact`
points `sync.sh` at GitHub's "latest release" alias, so it always fetches
whichever `kruddage/s7` release is newest; `sync.sh` fetches the artifacts below
into this directory and verifies each against the `.sha256` sidecar published
beside it in that release:

| Artifact | Role |
|---|---|
| `s7.h` | public header — consumers `#include "s7.h"` (this dir is on their include path) |
| `libs7-linux-x86_64.a` / `libs7-windows-x86_64.a` | native static library, linked into every native binary and the `krudd` host tool |
| `libs7-wasm32.a` | wasm static library (built with `emcc`/`emar`), linked into the single WASM module |
| `krudds7-linux-x86_64` / `krudds7-windows-x86_64.exe` | standalone s7 CLI — the `kruddmake` bootstrap/oracle interpreter (`krudds7 FILE`) |

`s7.h` and `libs7-wasm32.a` are target-independent and shared by every host.
The native library and CLI are host-specific: `sync.sh` fetches the Linux pair
by default and switches to the Windows (MinGW-w64) pair on a MINGW*/MSYS*
`uname -s` (see `S7_NATIVE_LIB_ASSET_WINDOWS` / `S7_CLI_ASSET_WINDOWS` in
`s7.artifact`).

s7 keeps its upstream `0BSD` notice (see `LICENSE.s7`); none of these artifacts
carry the project's `GPL-2.0-or-later` line — that marks our own files.

s7 is used in two roles: as *build authority* (linked into the `krudd` host tool
in `../krudd.c`, and the `krudds7` CLI that runs the `kruddmake` `.scm` scripts)
and as the in-process scripting/scene-description language of the engine runtime
(the `script` library and its callers), compiled into both the native binary and
the browser's single WASM module.

## Fetch

`../krudd.sh` and `../kruddmake/run-tests.sh` source `sync.sh` before linking
anything that touches s7 — including the `krudd.sh` bootstrap itself, which needs
the library before the `krudd` host tool exists. The artifacts are **not
committed** (see the repo `.gitignore`), so a fresh checkout fetches them on the
first build; `sync.sh` is idempotent, so once each artifact is present and
matches its sidecar checksum, no network I/O happens on later runs. The download
host is `github.com`, reachable from CI and normal dev machines.

Because "latest" tracks `kruddage/s7`'s release-please automation, a fetch can
briefly land in the gap between a new release being cut and its build workflow
finishing the asset upload (the release exists but an asset request 404s);
`sync.sh` retries a fetch a few times, 20s apart, before giving up, so that gap
doesn't fail the build outright.

The native build wires the prebuilt archives in through `kruddmake/ninja.scm`:
`$s7nativelib` rides after the engine archives on every native executable link
(as a static archive the linker pulls only referenced members, so s7-free
binaries are unchanged), and `$s7wasmlib` links into the WASM main module.

## Pin

| Field    | Value |
|----------|-------|
| Version  | s7 **10.8** (upstream `S7_VERSION`/`S7_DATE`, 17-Apr-2024; provenance only, may lag the actual latest release) |
| Release  | always the **latest** `kruddage/s7` release (see `S7_BASE_URL` in `s7.artifact`) |
| License  | 0BSD (Zero-Clause BSD) — permissive, zero conditions |
| Upstream | [S7 Scheme][upstream] |
| Source   | https://github.com/kruddage/s7 (release-please semver off Conventional Commits) |

There is no tag to bump: `sync.sh` re-fetches each asset's `.sha256` sidecar on
every run and re-downloads whenever the cached artifact no longer matches it, so
a new `kruddage/s7` release is picked up automatically the next time `sync.sh`
(or `krudd.sh`) runs. Each download is still checksum-verified against its
published sidecar, so a corrupted or truncated fetch fails loudly — but a
*bad* `kruddage/s7` release (one that publishes broken artifacts with a
matching sidecar, as v0.4.0's Windows build did) will now reach every build
immediately instead of staying quarantined behind a pinned tag.

[upstream]: https://ccrma.stanford.edu/software/snd/snd/s7.html

## Local patches

None here anymore. s7 used to carry a wasm-only `KRUDD-LOCAL PATCH` in this
repo's `s7.c` (an `indirect call signature mismatch` trap in the fx optimizer's
`p_pi` branch on any `(string-ref name 0)`, invisible to the native suite). That
patch now lives in `kruddage/s7` and is baked into `libs7-wasm32.a`, so the
silent re-vendoring footgun — a checksum-only bump quietly reverting to pristine
upstream — is gone. Any patch change is a `kruddage/s7` release and reaches this
repo as a tag bump. Still verify a wasm behavior change in a **browser**; the
native suite cannot catch a wasm-only signature mismatch.

## License compatibility

0BSD combining into a `GPL-2.0-or-later` work is the standard permissive-inbound
case, and 0BSD imposes *no* downstream conditions (not even attribution), so
there is nothing to reconcile. Because s7 is third-party it sits outside the
project's CLA / copyright-assignment surface, and 0BSD poses no obstacle to the
commercial license offered alongside the GPL build.

## Build-time feature configuration

The `kruddage/s7` release builds the libraries with `WITH_C_LOADER=0` (drops the
`dlopen`-based external-C-library loader — unneeded, unwanted surface for a build
tool) and `WITH_MAIN=0` (a library, not a REPL binary). `../krudd.sh` passes the
same `-DWITH_C_LOADER=0 -DWITH_MAIN=0` when compiling `krudd.c` so its view of
`s7.h` matches how the linked library was built. s7 is upstream code, so the
engine's warning flags are not enforced against it.

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
