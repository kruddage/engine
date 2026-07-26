# krudd2 carry audit

What the C/Scheme engine knows that the Rust/TypeScript rewrite should not have to
rediscover. Written for [#813](https://github.com/kruddage/engine/issues/813), which gates
the delete step of [#812](https://github.com/kruddage/engine/issues/812).

This note lives outside `krudd/` on purpose. Once #814 lands, the tree it describes is
gone, and reading the old code means reading git history. Everything here is written to be
usable without that.

Each verdict is one of:

- **carry** — the concept is ours, nothing in the new stack provides it, rebuild it.
- **subsumed** — wgpu, naga, the browser, or Cargo/npm already does this. Do not rebuild.
- **drop** — the concept was load-bearing only for the C/Scheme architecture. It leaves
  with it.
- **defer** — real, but not the spine. Recorded so the next initiative does not start cold.

Where a verdict is **carry**, there is a tracking issue; the reasoning stays here, the work
lives there.

---

## Render stack

### `render/frame_graph/` — 623 lines of implementation, 686 of tests

**Verdict: carry.** wgpu hands you a device, a queue, pipelines and command encoders. It
has no pass DAG, no transient resource lifetimes, and no barrier insertion. This is the one
render module that is entirely ours.

Detail sufficient to rebuild without the C:

**Object model.** A graph owns a fixed array of resources and a fixed array of passes
(`FG_MAX_PASSES` 64, `FG_MAX_RESOURCES` 64, `FG_MAX_PASS_DEPS` 16 per pass). A resource is
either *transient* — the graph creates and destroys its texture — or *imported*, meaning the
backbuffer: the graph binds it and never owns its storage. A pass declares a name, a read
set, a write set, an execute callback, and per-attachment load/clear config.

**Compile is three passes over the declarations:**

1. *Reference-count cull* (the Frostbite scheme). Count readers per resource; a pass's
   `ref_count` is the sum of reader counts over the resources it writes. Any pass that
   writes something nobody reads is dead — queue it, kill it, decrement the reader counts of
   everything it read, and propagate: a writer that drops to zero readers is now dead too.
   Two rules make this terminate correctly: a pass with **no writes at all** is a terminal
   consumer and is never culled, and an **imported resource carries an implicit external
   reference** (`reader_count` seeded to 1) so the pass producing the backbuffer survives
   even though no in-graph pass reads it.

2. *Topological sort* — Kahn's BFS. An edge runs from the pass that writes a resource to
   each pass that reads it; one edge per writer even if a pass reads the same resource
   twice. Result is `sorted[]`, the execution order.

3. *Lifetime computation* in sorted order. For each resource record `first_write` and
   `last_use`. The subtle rule, and the one that caused a per-frame leak before it was
   fixed: **a write counts as a use.** A pure attachment — a depth buffer nothing samples —
   is written and never read, so computing `last_use` from reads alone leaves it allocated
   forever, allocating a fresh one every frame. Fold writes into `last_use`.

**Execute** walks `sorted[]` and for each pass:

- creates textures for transients whose `first_write` is this pass (imported: never
  created);
- for each read whose writer ran earlier, emits a write→read barrier
  (`FRAGMENT`→`FRAGMENT`, `SHADER_WRITE`→`SHADER_READ`);
- builds the render pass descriptor **from the declared writes** — a depth-format write
  becomes the depth attachment, others become color attachments in declaration order, each
  carrying this pass's load op and clear value. The graph begins and ends the render pass.
  It does not wrap draw calls;
- invokes the execute callback with a lent context;
- destroys transients whose `last_use` is this pass (imported: never destroyed).

**MSAA resolve targets are modelled as flagged writes.** `pass_set_resolve(pass,
color_index, target)` appends the resolve target to the write set with
`write_is_resolve[j] = 1`. That one trick lets the existing cull, sort and lifetime
machinery treat a resolve target like any other produced resource — readers depend on the
producing pass, storage frees after the last read — while execute emits it as
`color[rc].resolve_target` rather than as a new attachment. Worth reproducing; it is a lot
of behaviour for very little code.

**The lend rule is the part that becomes a type.** Today `fg_pass_ctx` carries a comment:
*borrowed, not kept; valid only for the duration of the callback; never cache it or the
handles it returns across frames.* In Rust that comment is a lifetime parameter, and
violating it is a compile error rather than a use-after-free next frame. This is the single
clearest example of the rewrite buying something the old tree could only ask for politely.

The companion rule is the seam for persistent resources: pipelines and static mesh buffers
outlive a frame, so they are created **once, off-frame, against the device directly**, never
through the lent context. The scene renderer holds no persistent `gpu_api` pointer at all —
it resolves the device only in init/shutdown and in a pre-pass warm-up. Keep that
separation; it is what makes "the graph owns the frame" true rather than aspirational.

Tracking: #823.

### `render/renderer.scm` — the GPU abstraction (338 lines)

**Verdict: mostly subsumed, with two carries.**

Subsumed: the handle types, formats, topologies, load/store ops, buffer usages, pipeline
descriptors, render pass descriptors, barrier stage/access masks and the `gpu_api` vtable
are all wgpu's `Features` / `Limits` / descriptor types, expressed better. Do not port them.

The `clip-z-zero-to-one` capability flag disappears outright: it exists because WebGL's NDC
z runs `[-1, 1]` while WebGPU/D3D/Metal run `[0, 1]`, so a GL-convention projection needed
adapting per backend. wgpu normalizes to `[0, 1]`. Build projections for `[0, 1]` and delete
the concept — but *know* it existed, because a projection matrix copied from a GL tutorial
will silently clip its near half and look like a depth bug.

Carry #1: **`frame_end` as a distinct call from submit.** The comment on it is the most
expensive lesson in the file. A frame is not one command buffer — the frame graph submits,
the GUI overlay submits, an open preview panel submits again, and nothing bounds the count.
So a backend holding a per-frame resource cannot release it at submit, because submit is not
the end of anything. The WebGPU backend holds the canvas surface texture; releasing it at
submit meant the next subsystem to draw acquired a second, blank one, and the canvas showed
only whatever drew last. There is deliberately no matching `frame_begin`: acquisition is
lazy, done by the first pass that names the backbuffer. Any multi-submitter frame in krudd2
inherits this bug unless the surface texture is released at an explicit frame boundary.

Carry #2: **the opaque texture handle pair** (`texture_handle` / `cmd_bind_texture_handle`).
A UI layer compositing a render-target texture through its own quad batch needs to hold a
reference to that texture across a language boundary as a plain integer. Same shape in
krudd2: TypeScript holding a reference to a Rust-owned GPU texture. The contract that
matters is the safety half — the same live texture always hashes to the same id, and an id
whose texture was destroyed resolves to *nothing* rather than to whatever recycled the slot.
A generational index in Rust gives this for free.

### `render/shader/` — the DSL and its transpiler (1033 lines)

**Verdict: split.** The lowering half is subsumed; the metadata half is a carry.

Subsumed: parsing a shader DSL and emitting GLSL ES 300 and WGSL from one source. That is
naga's job, and WGSL is the source language. Delete the transpiler.

Carry: the `(edit ...)` clause. A uniform block field is declared
`(base_color vec4 (edit color))` or `(roughness float (edit range 0 1))`, and the editor
reads that metadata to build the right inspector control — a color swatch, a bounded slider
— with `(default ...)` supplying the starting value. GLSL and WGSL emission both ignore it
entirely; it exists purely so the authoring UI is derived from the shader rather than
hand-written beside it. **WGSL has no equivalent and naga will not give you one.** Whatever
carries material parameters in krudd2 needs somewhere to hang this, and the vocabulary
generalizes: the same `(params (NAME TYPE (edit ...) (default ...)))` shape is used by
shaders, entity scripts, mesh scripts and texture scripts alike. One vocabulary, four
consumers — see *Parameterized assets* below.

### `render/null/` — the null backend (438 lines, 432 of tests)

**Verdict: carry, reframed.** 813 asks whether a null backend still earns its place when
there is no backend abstraction left to test. The backend-abstraction argument is indeed
gone. The one that survives is different and stronger: `renderer_null.h` is not a stub, it
is a **recording oracle**. Every entry point appends a `struct gpu_call_record` to an
in-memory log — `pipeline_create` records its color format count and whether each stage had
source, `begin_render_pass` records its color count and whether color 0 carried a resolve
target, `texture_create` records format/width/height/mips/has-initial-data. A test resets
the log, drives the graph, and asserts on the exact call sequence.

That is how you unit-test a render graph — pass culling, execution order, transient
allocation and free points, barrier emission, resolve wiring — with no GPU, no adapter, and
no flakiness, which is exactly what CI wants. wgpu has no true null backend; the equivalent
is a recording implementation of whatever trait the graph talks to. Build it early: the
frame graph's tests are worth more than the frame graph, and they only exist because this
did.

### `render/scene_renderer/` — 3635 lines

**Verdict: out of scope for the spine, but four domain lessons are worth having.**

1. **Pipeline variants are not optional.** The forward pipeline needs a multisampled twin
   because the pass it runs in is multisampled, and the bloom composite needs a
   backbuffer-targeted twin because the backbuffer carries the backend's emulated depth
   while an offscreen target carries none. WebGPU validates a pipeline against the pass it
   runs in, so one pipeline genuinely cannot serve both. wgpu validates the same way. Plan
   for a pipeline cache keyed by (shader, target state) from day one rather than
   discovering the need three post-effects in.

2. **Three blur targets, not a two-buffer ping-pong.** extract→a, blur-H a→b, blur-V b→c,
   composite reads c. Every hazard is then a plain read-after-write, which is the only
   ordering a simple graph tracks. A ping-pong introduces a write-after-read the graph
   cannot see. Cheap insurance.

3. **Every fallback path is null-checked into a no-op.** If the bloom pipelines fail to
   compile, the tick falls back to a direct forward-to-backbuffer pass with no bloom; if the
   outline pipelines fail, the outline is silently skipped. A post-effect is a pure add-on
   that can never break ordinary rendering. Keep that discipline — in Rust it is an
   `Option<Pipeline>` and a `let else`.

4. **Bind a dummy, never nothing.** A 1×1 depth texture cleared to 1.0 is bound wherever no
   real shadow map exists, so the shadow sample reads "unoccluded" instead of garbage, and it
   must be a *depth* texture because WebGPU rejects a color texture in a depth slot (WebGL
   was untyped and did not care). Same for the color dummy. wgpu is as strict as WebGPU
   here.

### `render/particles/` — 296 lines

**Verdict: defer, but preserve the seam.** CPU-integrate a fixed pool, bake camera-facing
quads, draw the batch with one non-indexed draw. It leans on nothing but a dynamic vertex
buffer and `cmd_draw`, so it runs on every backend — which is precisely right for a
WebGL2-first rewrite. The design note it carries is the useful part: the *only* thing that
changes when a compute path arrives is the producer of the per-particle buffer. Keep
`update()` (fills the buffer) separate from `render()` (draws it), pick the producer by
capability, and the compute upgrade touches one function.

---

## Beyond the render stack

813 scopes the audit to render. Walking the rest of the tree turned up four concepts that
are at least as valuable, and one of them (the interface-spec codegen) bears directly on
#817.

### The interface spec as the single source of truth

`renderer.scm` is not documentation of `renderer.h` — it *is* `renderer.h`, lowered by
`kruddmake/introspect.scm` at build time. The same pattern generates the s7 image, the
shell template's version substitutions, and the editor layout's JSON. The rule that makes it
work is declared in `manifest.scm`: **a directory that declares a build fact gets a spec**,
and that one declaration is both what the generator runs and what the build watches for
changes — so a source cannot be generated from without also being rebuilt for. Those two
lists drifting apart was a real bug class (#779, #787) before the rule existed.

This is the concept that maps most directly onto #817. The Rust/TS boundary has exactly the
failure mode a hand-maintained pair of definitions produces: a `#[repr(C)]` struct on one
side and a `DataView` offset table on the other, drifting silently, corrupting typed-array
reads in a way that looks like a renderer bug. One spec generating both sides — plus the
build watching it — is the fix, and this tree already proved it out.

### Parameterized assets, and one `params` vocabulary across all of them

Every mesh in the engine is a `(mesh NAME (params …) (generate () …))` script. There is no
hardcoded C mesh generator left — box, sphere, plane, pyramid, grid, the revolved
cylinder/cone/capsule/disc/torus, the parametric superquadric and heightfield are all
scripts. Textures and sounds work the same way. And the same `params` clause, with the same
`(edit …)` / `(default …)` vocabulary, appears on shaders, entity scripts, mesh scripts and
texture scripts alike.

The payoff is the per-entity override. The world stores tight-packed override bytes per
entity for script, material, mesh and texture params, and a `len` of 0 means "no override —
use the declared defaults". So two entities share one mesh asset and draw at different
sizes; two entities share one material and draw in different colors; two entities share one
material and bake its procedural texture at different scales. Assets stay shared; variation
lives on the entity. That is a material-instance system generalized to geometry, pixels and
behaviour, and it is genuinely ours — nothing in the new stack provides it.

Two supporting details worth keeping. Caching is keyed by what the thing actually depends
on: a param-less mesh is a pure function of nothing, so it is parsed and generated **once
per exact source text** and cached forever, while a parameterized mesh caches per
(source, params). And the editor's hit-test generates its candidate geometry **through the
same path with the same overrides** as the draw — so a resized box's hitbox matches the box
that was drawn, not the default one. Sharing that code between the wasm viewport and the
native shell is why they never drifted.

### World storage: struct-of-arrays, topological order, tombstones

`struct world` is flat parallel columns keyed by a dense entity id — `alive`, `mask`,
`parent`, `local`, `world_xform`, `name_off`, the various `*_ref` columns. Hierarchy is the
`int32_t parent[]` column (`-1` = root), never pointers. Two invariants carry the weight:

- **Entities are stored in topological order** (parent index < child index), so
  `world_propagate_transforms` resolves every world transform in one forward pass — no
  recursion, no visited set, no sort. Creation appends, which preserves the order for free.
- **Destruction tombstones, it does not swap-remove.** A slot (and its descendants) is
  marked dead without shifting any index, so parent references stored in surviving entities
  stay valid. A swap-remove would shift indices and silently corrupt the hierarchy — the bug
  would present as an unrelated entity teleporting.

812 already puts ECS storage arrays on the Rust side of the boundary. This is the layout to
put there, and the two invariants are the reason it is more than "an array of structs, but
sideways".

### Editor chrome as data

`core/editor_layout.scm` describes the entire editor shell — menus, toolbar, docks,
status-bar fields — as one data tree. Two hosts read it and **neither hard-codes a menu,
dock or toolbar literal**: the Qt shell walks it into widgets, and the browser shell
serializes it to JSON and builds DOM from it. A dock added to the spec reaches both hosts
with no host-specific edit.

krudd2's editor is HTML, so the Qt half is moot — but the rule survives contact: the editor's
structure is data, action ids are opaque strings the host wires (an unrecognized id degrades
to a placeholder rather than failing), and the spec lives with the engine rather than beside
any one host.

---

## Non-code principles

For #812's principles section.

- **Test parity is a norm, not an aspiration.** `fg_test.c` is 686 lines against 623 of
  implementation; `renderer_null_test.c` is 432 against 438; `kgui_assets_test.c` is 1409.
  The tree held roughly 1:1 on its core modules. That ratio is a decision that has to be
  made deliberately at the start, because it cannot be retrofitted.
- **The recording-oracle pattern is what made that ratio affordable.** Testing a renderer by
  asserting on a call log, rather than on pixels, is why the frame graph has more test than
  implementation. Build the oracle before the thing it tests.
- **Prefer uniformity over individual taste.** From `CODING_STANDARD.md`: this engine is
  built by many hands and many agents, most of which never meet; the code is the only thing
  they share, so the code carries the coordination. When two forms are both correct, the
  canonical one wins, because it costs no one a judgment call. In krudd2 that means
  `rustfmt` and `clippy` at their defaults, and a TS formatter config nobody argues about —
  the same rule, enforced by tooling that already exists.
- **Doc comments are linted, not hoped for.** `.scm` files carry `;;!` doc comments checked
  in CI by `lint-scm-comments.py`, mirrored in a tracked pre-commit hook so a violation is
  caught locally. `rustdoc` plus `#![warn(missing_docs)]` is the direct equivalent.
- **A tier order that the layout on disk agrees with.** `manifest.scm` is the authoritative
  dependency order — a module may only reach for one listed above it — and the directory
  grouping mirrors it, so the ordering is visible without reading the list. `abi/` is
  strictly vtables and forward-declares any type it does not own rather than reaching down
  into an implementing module; a module exports an `include/` directory holding *exactly*
  what its consumers need, everything else stays private at the module root; `shell/` is
  last because a shell may reach for anything and nothing may reach for a shell. This is
  812's "package based, avoid API leaks" already worked out, and Cargo plus TS exports can
  enforce mechanically what the manifest enforced by convention.
- **A real-time path allocates nothing and calls no interpreter.** The mixer sums voices
  into a stereo float buffer with no Scheme, no allocation and no locks in `mixer_render`,
  specifically so it can run from an audio callback. It also splits identity from variation:
  a *blob* is the baked waveform, a *voice* is one playing instance carrying only gain, pan
  and rate — the live knobs that must never trigger a re-bake. Voice handles carry a
  generation so a stale handle to a reused slot is inert. Same split, same discipline, when
  audio arrives on the Rust side.
- **Do not trust an instrument that cannot fail loudly.** `tools/render-diff` exists because
  headless Chrome composites WebGPU canvases as blank while reporting a live device — and
  WebGL captures fine in headless, so the obvious sanity check actively confirms a broken
  instrument. The harness therefore has no `--headless` flag at all and re-execs under
  `xvfb-run`; a mode that silently reports "nothing rendered" is worse than no harness. It
  also injects a shim around `requestDevice` before any page script runs, because a
  validation failure otherwise surfaces as a blank canvas plus a message that never reaches
  the page console. WebGL2-first defers the WebGPU half of this, but the design rule is
  permanent.

---

## What leaves with the tree

- **The subsystem manager and the plugin vtable ABI.** Runtime service discovery by string
  name (`subsystem_manager_get_api(mgr, "renderer")`) exists because every module compiles
  into one wasm blob with no linker-level dependency graph. Cargo and TS imports *are* that
  graph, checked at compile time. The one idea worth remembering is async readiness — a
  subsystem registers, signals ready later, and callers queue a callback rather than
  polling; the GPU device coming up after the wasm module does is the same shape, and it is
  a `Promise` / `OnceCell` in the new stack rather than a registry.
- **The s7 Scheme runtime, embedded as both build language and scripting layer.** 812 rules
  on this directly. The build side becomes `cargo xtask`; the scripting side becomes
  TypeScript.
- **`kruddmake`'s Ninja emitter and transitive include/link resolver.** Cargo and a TS
  bundler. The *manifest* concept survives (above); the emitter does not.
- **kruddgui**, the immediate-mode GUI: batch, font atlas, text edit, input. 812 rules the
  editor GUI is HTML. Worth noting the batch's one non-obvious idea in case an in-game GUI
  needs it later — a single vertex buffer split into draw commands only where the clip rect
  or the bound texture changes, so a scrolling panel flushes as a couple of scissored draws
  out of one VBO.
- **The Qt shell, the Vulkan backend, the WebGPU backend.** 812's non-goals; Tauri hosts the
  same web build and WebGL2 is the only path for proof of life.
- **`kruddboard`'s markdown parser** and the chess/tictactoe sample games — demos of the
  authoring surface, not engine concepts.

---

## Tracking issues

Concepts marked **carry** each have an issue under #812:

| Concept | Issue |
|---|---|
| Frame graph — pass DAG, transients, barriers, resolve-as-write | #823 |
| Interface spec as the single source of truth for the boundary | #824 |
| Parameterized assets and the `params`/`edit` vocabulary | #825 |
| Recording backend as the headless GPU test oracle | #826 |
| Screenshot oracle and the instruments-that-lie rule | #827 |
| World storage — SoA, topological order, tombstones | #828 |
| Module tier order and public-surface enforcement | #829 |
| Editor chrome as data | #830 |
