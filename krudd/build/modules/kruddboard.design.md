<!-- SPDX-License-Identifier: GPL-2.0-or-later -->

# Design: lowering kruddboard into Scheme

Status: **draft for review** — no code yet. This is the Track B deliverable: the
design + spike plan for the outbound Scheme→ImGui/engine binding that a full
`kruddboard.scm` port depends on. Read this before committing time to the port.

## TL;DR

Moving `kruddboard.cpp` (1850 lines) into `kruddboard.scm` is **not a port, it is
a new bidirectional binding layer.** Every migration so far
(`md_parse.scm`, `primitives.scm`, `math.scm`) is pure logic that **C calls into
and gets data back from**. kruddboard is the inverse: every frame it calls **out**
to ImGui and nine engine vtables, chases raw `world` struct memory, and hands C a
per-frame draw callback. The binding generator in `introspect.scm` explicitly
**fails loud** on exactly those things — pointers, callbacks, structs-into-Scheme.

So the real work is building the outbound seam. This doc specifies it, and a
~7-primitive spike that de-risks the single biggest unknown (per-frame `s7_call`
cost) before anyone ports a single tab.

## What kruddboard actually touches (measured, not guessed)

| Surface | Count | Examples |
|---|---|---|
| Distinct `ImGui::` calls | ~65 | `Begin/End`, `BeginTable`, `Selectable`, `InputTextMultiline`, `Combo`, `BeginTabItem`, `CollapsingHeader` |
| ImGui flag/enum families | ~24 | `ImGuiWindowFlags_*`, `ImGuiTableFlags_*`, `ImGuiCol_*`, `IM_COL32` |
| Engine vtable methods | 40 | across `log stats asset asset_mut backend scene edit camera` + `subsystem_manager` |
| Raw `world` struct fields | 9 | `mask name_off world_xform names alive parent local render_ref count` |
| `ImDrawList` ops (gizmo) | 4 | `AddLine AddCircle AddCircleFilled AddRectFilled` |

Plus: an emscripten keydown callback (backtick toggle, Ctrl+Z/Y undo) and a
`register_panel(name, C-fn-ptr, userdata)` registration.

## Why the existing generator does not apply

`krudd-embed-scheme-module` (introspect.scm) marshals **a Scheme return value into
a C struct array** — one direction, C→Scheme→data. Its locked vocabulary rejects
"pointers/opaque handles, callbacks, structs passed INTO Scheme, unions, bitfields,
heap/varlen, nested vectors" (introspect.scm:280-283). kruddboard is made of all of
those. `kruddboard.scm` is therefore **not an ABI module** like `md_parse.scm`; it
is a **runtime image** like `runtime.scm` — baked with `krudd-embed-file` and
`script_eval`'d, not run through the binding generator. This distinction drives the
whole design.

## Architecture

```
                 wasm-only "kruddboard host" (C++)
  ┌───────────────────────────────────────────────────────────┐
  │ init:  resolve 9 vtables via subsystem_manager_get_api     │
  │        s7_define_function() every ig-* / engine-* primitive │
  │        s7_define_constant() every ImGui flag (from headers) │
  │        script_eval(kruddboard_scm)   ; bake+load the image  │
  │        imgui->register_panel("##kruddboard", trampoline)    │
  │                                                             │
  │ trampoline(frame):  s7_call_with_catch(kruddboard-draw)     │
  └───────────────────────────────────────────────────────────┘
         │ primitives (out)              ▲ (kruddboard-draw) each frame
         ▼                               │
  ImGui + engine vtables          kruddboard.scm  (the UI, in Scheme)
```

Key placement decision: **the primitives cannot live in `script.c`.** `script.c`
is in `modules/core` and compiles **native + wasm**; ImGui is **wasm-only**. So the
binding lives in a dedicated **wasm-only host module** (the shrunken successor to
`kruddboard.cpp`) that registers primitives against the process-global
`script_s7()`. `script.c` stays clean — core, both targets, one primitive
(`krudd-log`).

### Frame hook (inversion of control)

Today: `register_panel("##kruddboard", draw_board /*C*/, nullptr)`.
Design: register a C trampoline that does `s7_call_with_catch(s7, kruddboard-draw,
nil)` — the exact move `script_tick()` already makes for `(tick)`
(script.c:79-88). The catch is mandatory: a Scheme error mid-frame must log and
draw a fallback, never unwind through the emscripten main loop.

## The ImGui binding vocabulary

Mirror `introspect.scm`'s philosophy: a **locked, enumerated** set of exactly the
calls kruddboard uses, each `s7_define_function`-registered, anything else a loud
error. Not "bind all of ImGui." Prefix `ig-`.

The eight non-obvious problems and their rulings:

1. **Booleans gate blocks.** `if (BeginTabItem("X")) {…; EndTabItem();}` becomes
   `(when (ig-begin-tab-item "X") … (ig-end-tab-item))`. Primitives return s7
   booleans; control flow is native Scheme. Clean.

2. **Output/mutation params → return values.** `Checkbox(l,&b)`,
   `InputFloat3(l,v3)`, `Combo(l,&i,…)` mutate in place. Scheme has no `&`. Ruling:
   the primitive takes the current value(s) and **returns `(changed? . new-values)`**,
   e.g. `(ig-input-float3 "Pos" x y z) -> (#t nx ny nz)`. State is threaded
   explicitly by the caller — strictly cleaner than kruddboard's hidden `static`s.

3. **Label lifetime is a non-issue.** `s7_string(arg)` is a `const char*` valid for
   the call; ImGui copies labels into its ID stack / draws immediately. Safe.

4. **The 64 KB InputText buffer stays C-owned.** ImGui edits a persistent mutable
   buffer in place across frames (stb_textedit). Ruling: keep an id-keyed C buffer
   in the host; expose `(ig-input-text-multiline id)` operating on it, plus
   `(ig-text-get id)` / `(ig-text-set id s)` to sync whole strings at
   load/save boundaries. Matches how md editing already reloads on selection change.

5. **Flags/enums are integer constants, sourced from ImGui headers.** At init the
   host `s7_define_constant`s each one from the real enum value
   (`ig-window-no-title-bar`, `ig-table-borders`, …) so they can never drift from
   the headers. Combine in Scheme with `(logior …)`.

6. **`ImVec2`/`ImVec4` pass as flat floats.** `(ig-set-next-window-pos x y)`,
   `(ig-text-colored r g b a s)`. No struct marshaling.

7. **Draw list = implicit background list.** `(ig-bg-add-line x0 y0 x1 y1 col th)`,
   `(ig-bg-add-circle-filled x y r col)`, etc. `col` is packed: `(ig-col32 r g b a)`.

8. **Drag-drop payloads are one 4-byte int.** `(ig-set-drag-payload "ASSET_ID" id)`
   / `(ig-accept-drag-payload "ASSET_ID") -> id | #f`. Narrow, exact.

IO/viewport queries return small lists: `(ig-display-size) -> (w h)`,
`(ig-mouse-pos) -> (x y)`, `(ig-mouse-clicked? btn) -> bool`,
`(ig-want-capture-mouse?)`, `(ig-viewport-work-size) -> (w h)`.

## The engine vtable / read-model binding

The host resolves all nine vtables once at init (as `kruddboard_plugin_entry`
already does) and closes over them.

**Reads — snapshot the collections once per frame, accessors for scalars:**

- Scalars: `(stats-fps-avg)`, `(stats-frame-ms)`, `(edit-can-undo?)`,
  `(edit-undo-label)`, `(camera-view-proj) -> (16 floats)`, `(camera-eye) -> (x y z)`.
- Collections marshaled C→s7 once per frame (bespoke — the generator won't do
  structs-into-Scheme, but it is small and is just md_parse's marshaling reversed):
  - `(log-history) -> ((level . text) …)`
  - `(subsystems) -> ((name api? tick? wasm-size) …)`
  - `(assets) -> ((id path type kind state size refs read-only? origin) …)`
  - `(world-entities) -> ((i alive? name parent mask pos rot scale render-ref) …)`

Marshaling per frame (not per row via primitives) is also the main perf lever —
one crossing per collection instead of one per `TableSetColumnIndex`.

**Writes — one thin primitive per mutation:**

- scene: `entity-create`, `entity-set-name`, `entity-set-transform`,
  `entity-set-selected`, `entity-set-render-ref`, `entity-destroy`
- asset: `asset-create`, `asset-set-data`, `asset-set-decl`, `asset-destroy`,
  `asset-get-data`
- edit: `edit-begin`, `edit-commit`, `edit-undo`, `edit-redo`
- backend: `backend-caps`, `backend-persist`, `backend-delete`
- camera: `camera-set-viewport`

## State ownership

kruddboard's `static` locals (`filter`, `autoscroll`, the New-Asset form fields,
`g_asset_sel`/`g_entity_sel`, `g_edit_id`, gizmo drag state) become **explicit
module-level state in `kruddboard.scm`** — the hidden statics made visible. The
only exception is the 64 KB edit buffer, which stays C-owned (§4).

Stays in C (host), at least initially: the **emscripten keydown callback** (it is a
browser event, not an ImGui call) and the panel-registration lifecycle (the
existing lazy "register once the imgui api appears" in `kruddboard_tick`).

## Performance — the thing the spike must answer

A full board frame is a few hundred ImGui calls. Routing each through s7 foreign-
function dispatch at 60 fps is the primary risk and the **one number we must get
before porting 1850 lines.** Levers already in the design: per-frame collection
snapshots (not per-row calls); flat-float args (no allocation); returning lists
only where edits happen. If dispatch still dominates, the fallback is batching — a
Scheme list of draw ops flushed to C once per frame — but we do not build that
until the spike says we must.

## Proving & rollout

UI can't be diffed byte-for-byte like `md_parse.scm` was against `md_parse.c`.
Instead: **keep the C board behind a compile flag and run both side by side**, the
board dispatching C-tab or Scheme-tab per flag so partial migration ships. Order by
difficulty:

1. **KRUDD tab** — stats / subsystems / log, all read-only. Exercises text, tables,
   collapsing headers, and the log-history snapshot. Validates the whole seam.
2. **World tab** — entity list, inspector, gizmo, mutations, the `world` snapshot.
3. **Assets tab** — InputText buffer, create/delete, persistence. Hardest; last.

Each tab is a landable PR.

## Build wiring

`kruddboard.scm` lands under `krudd/build/modules/` beside `md_parse.scm`, but is
wired like `runtime.scm`, **not** like `md_parse.scm`: one `krudd-embed-file` call
in `ninja-generate-codegen` (ninja.scm:512 pattern) baking it to
`kruddboard_scm.h`, which the host `script_eval`s at register time. No
`krudd-embed-scheme-module` — there is no ABI header to generate.

## The spike (smallest end-to-end proof)

Goal: `(kruddboard-draw)` in Scheme renders **only the Frame Stats section**, live
in the browser, and prints a measured frame time.

Minimal surface:
- host module: trampoline + bake/load `kruddboard.scm`
- `(ig-begin id flags)` / `(ig-end)`
- `(ig-collapsing-header label flags) -> bool`
- `(ig-text s)` — Scheme does the `string-append`; primitive takes the final string
- `(stats-fps-avg)` / `(stats-frame-ms)` / `(stats-frame-count)`

~7 primitives + 1 trampoline + 1 host + a ~10-line `.scm`. If it renders and the
frame time is acceptable, the vocabulary scales linearly from here. If the frame
time is bad, we learned it for a day's work instead of after porting the whole
window.

## Open questions for review

1. **Host module identity** — grow the primitives inside today's `kruddboard`
   module, or a separate `imgui_script` module reusable by future Scheme UIs?
2. **Gizmo math** — keep the pure matrix/quaternion helpers in C (called by Scheme),
   or move them into `kruddboard.scm`? They run per-mouse-move, so this is really a
   sub-question of the perf spike.
3. **Constant sourcing** — `s7_define_constant` from ImGui enums at init (never
   drifts, costs startup) vs. a generated constants `.scm` (static, reviewable).
4. **Flag day vs. flag-gated** — is the compile-flag dual-board worth the
   complexity, or do we port tab-by-tab on `main` accepting brief regressions?
