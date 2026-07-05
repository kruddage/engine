# Changelog

All notable changes to KRUDD are recorded here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project aims to
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Entries are grouped by theme — features and fixes that matter to users of the
engine — rather than by individual commit. Pre-`0.3.0` releases predate this file
and are summarized at a high level.

This file has a build-time consumer: kruddboard's "What's New" tab renders it
in-app, so a shipped change is visible to anyone running the engine, not just
to someone reading git history. The contents are baked into the WASM module at
build time, which makes this file the app's release notes — a change is only
visible in the "What's New" tab once it's written down here. (`release.yml`
still auto-generates GitHub Release notes from merged PR titles rather than
from `[Unreleased]`.) This file remains the release-notes source of truth:
every user-facing change is recorded here, in prose meant for someone using the
engine, before it lands anywhere else.

## [Unreleased]

## [5.0.0] - 2026-07-05

### Added

- **"What's New" tab in kruddboard** — a new leftmost tab renders `CHANGELOG.md`
  inside the running engine through the existing markdown stack, so shipped
  changes are visible in-app. The changelog is baked into the WASM module at
  build time.

### Fixed

- **Markdown rendering** — inline `**bold**` and `` `code` `` delimiters are now
  stripped from rendered output instead of showing through as literal markers,
  and bold/code runs are tinted so emphasis reads clearly with the default font.
  Improves both the "What's New" tab and the Assets markdown preview.

## [4.0.0] - 2026-07-05

### Added

- **Shader authoring in kruddboard** — the Assets tab "New Asset" form now offers
  a type picker (Text / Shader) and, for shaders, a stage selector; the stage
  drives both a derived file extension (`.vert`/`.frag`) and the asset's
  declaration. Authored shaders open in a plain source editor with editable
  stage/dialect metadata instead of the markdown editor/preview.
- **Editable asset declarations** — `asset_mut.set_decl` lets editors attach
  key/value declaration metadata (e.g. shader stage/dialect) to authored assets;
  `describe()` surfaces it, and synthesizes a shader's stage from its file
  extension when no explicit declaration is set (e.g. after a reload).

### Changed

- **Version string format** — the build-number suffix shown in the browser
  tab title and in-app header, and logged at engine init, now reads
  `4.0.0.7` (dot-separated) instead of `v0.3.0+7`.

## [0.3.0] - 2026-06-29

The plugin platform grows up: a real asset pipeline, a runtime entity/scene
system, and an in-browser authoring surface (kruddboard) land on top of the
0.2.0 plugin ABI.

### Added

- **Entity & scene runtime** — runtime entity system backed by `struct world`,
  with a versioned `.scene` v1 file format, a decoder plugin, and accompanying
  format docs. Scene entities use the shared `vec3_t`/`quat_t` math types.
- **Asset system** — catalog enumeration API with built-in primitives, stable
  catalog IDs, a typed decoder/codec registration API, and a mutable
  authored-asset API including a new `ASSET_TYPE_TEXT`. The asset inspector gains
  a type discriminator and a per-type describe hook.
- **Math types** — shared `vec3_t`/`quat_t`, plus `mat4` and camera helpers, used
  across the scene and renderer plugins.
- **Frame graph** — GPU lent at execute time, graph-owned render passes, and
  backbuffer import.
- **kruddboard authoring** — a tabbed editor window with an Assets tab and a
  dependency-free markdown parser (with an ImGui draw shim) for authoring,
  editing, and persisting markdown assets. Debug panels are consolidated into a
  single KRUDD tab, with a World tab placeholder for the entity view.
- **Local persistence** — a backend capability seam with a Local IndexedDB
  provider for persisting assets in the browser.
- **Text input** — a web text-input bridge wiring desktop and mobile (on-screen)
  keyboards into ImGui.
- **Build & version surfacing** — build number encoded after the version
  (`v0.3.0+N`) and shown in the shell title bar and header; commit-hash cache
  busting for the WASM and JS assets; a WASM size column in the subsystems table.

### Changed

- Relicensed to **GPL-2.0-or-later** (from the original MIT, via a brief
  LGPL-2.1-or-later interim). Proprietary/commercial use now requires a separate
  commercial license; external contributions require a CLA.
- Consolidated the three kruddboard panels into one tabbed editor window.
- Consolidated the GitHub Pages writes into a single deployer workflow.

### Fixed

- **Plugin loading** — load plugins sequentially in dependency order, wire
  WebGL/GLES symbols into the main module, count a plugin's own weak exports as
  self-resolved, and treat the main module's `GOT.func` address-of imports as
  provided.
- **Mobile & touch** — keep the subsystems panel on-screen on mobile and set
  `IsTouchScreen` so tabs respond to a single tap.
- **Input** — de-duplicate IME/composition text in the input bridge.
- **Shell** — flip the status pill to "running" on startup and include the error
  title when copying exception details.
- Replaced `EM_ASM_*` macros with `EM_JS` in side modules to avoid C
  preprocessor breakage, and hardened the asset API to reject `set_data` with
  NULL bytes but a non-zero size.

## [0.2.0]

The plugin era. The engine moved from a single binary to a modular host with a
stable WASM ABI.

### Added

- Plugin architecture: a stable WASM ABI, a subsystem manager, and a plugin
  loader that discovers engine services through vtables rather than named
  imports.
- Logging module with level filtering and ring-buffer history.
- Memory module: an allocator and a fixed-size pool allocator.
- Renderer scaffolding (null and WebGL backends) and an ImGui-based debug UI
  shell with an error overlay.

## [0.1.0]

Initial release — the engine heartbeat.

### Added

- `engine_init` / `engine_tick` / `engine_shutdown` lifecycle, with C driving the
  frame loop via `emscripten_set_main_loop`.
- Emscripten build producing `index.{html,js,wasm}`, served as a static site.

[0.3.0]: https://github.com/kruddage/engine/releases/tag/v0.3.0
[0.2.0]: https://github.com/kruddage/engine/releases/tag/v0.2.0
[0.1.0]: https://github.com/kruddage/engine/releases/tag/v0.1.0
