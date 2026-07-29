# @kruddage/editor

The KRUDD editor, as a TypeScript application.

```sh
pnpm --filter @kruddage/editor run dev        # the editor, against a built engine
pnpm --filter @kruddage/editor run test       # the component suite — no browser, no emsdk
pnpm --filter @kruddage/editor run test:e2e   # the browser suite — needs a built engine
pnpm --filter @kruddage/editor run build      # typecheck, then a production build
```

## What this is, at this stage

**The shell, the viewport, the inspector and the outliner.** The asset browser
and the build panel are still placeholders that say so on screen.

The frame — command bar, toolbar, rail, resizable docks around a viewport deck,
and a status strip — was built in
[#954](https://github.com/kruddage/engine/issues/954) before the boundary
existed, and it has not moved since. Every panel that has arrived has arrived by
adding a file and a line to `src/panels/index.tsx`, which is the property that
whole issue was about.

The panels that are still empty are empty in the shipped editor today too. Every
dock in `krudd/engine/core/editor_layout.scm` renders a heading and a one-line
blurb; this shell renders the same headings and the same blurbs, plus the issue
that fills each one in.

### The vocabulary

Settled in `src/shell/vocabulary.ts`, one line each, and checked by the suite so
it cannot quietly grow a synonym:

**command bar, toolbar, rail, viewport deck, outliner, inspector, assets,
console, build, status strip.**

Use these words in code, comments, commit messages and PR bodies. "Sidebar" is
not a rail and "tree" is not the outliner — a second name for one thing is how
six PRs end up describing four different layouts. #855 fixed the board's
vocabulary the same way and it is why board/node/wire/port/lane never became an
argument.

### A panel registers itself

```ts
registerPanel({ id: "outliner", title: "Outliner", home: "left", render: () => <Outliner /> });
```

The shell reads the registry and places what it finds, so
[#950](https://github.com/kruddage/engine/issues/950),
[#951](https://github.com/kruddage/engine/issues/951) and
[#952](https://github.com/kruddage/engine/issues/952) each add a file and a line
to `src/panels/index.tsx` and touch no part of the frame. `test/shell.test.tsx`
asserts it, because that criterion is the difference between three small PRs and
three PRs that each edit the layout everything else depends on.

**A panel is mounted once and moves.** There is no path in the shell that
constructs a second instance of one — #886's mockup grew three Inspectors that
drifted apart, and that is the outcome this rule makes impossible rather than
merely discouraged.

### The layout is remembered, and a stale one is survivable

Docks resize, collapse and restore; the arrangement persists across a reload and
View > Reset Layout puts it back. The interesting half is `reconcile()` in
`src/shell/layout.ts`: a reader's stored layout is always older than the build
reading it, so a panel that no longer exists is dropped, a panel the store never
heard of is placed at its home, and an unreadable store falls back to the
default. A shell that crashed on any of those is one a reader fixes by clearing
site data, which they will not know to do.

### What is wired, and what says "coming soon"

`reset-layout`, the panel toggles and the moves are real. **Every other menu
action shows a transient hint** — the label with its `&` mnemonic stripped, then
": coming soon" — which is exactly what `shell.html.in:1548` does today and what
the native host does for an id it does not recognise. That is what makes a
read-only shell honest rather than broken, and
[#948](https://github.com/kruddage/engine/issues/948) replaces the hint path
with #947's command layer without moving anything.

The menu set, the labels and the shortcuts were **mined from
`editor_layout.scm` and then the file was closed**. Nothing reads it at runtime
(#944's Q3); `test/commands.test.ts` carries the spec transcribed by hand so a
divergence is a decision someone made rather than one the suite let through.

### Container queries, not media queries

A panel answers to the frame it is in, not to the window, so a dock dragged
narrow reflows while the window stays wide. Every dock body is a query
container. #944 rules the phone out of scope and this goes in anyway: it costs
days now and a rewrite of every panel later. `test/styles.test.ts` fails the
build on a width media query, and on any font size fixed in pixels.

## The viewport, and the seam it sits on

[#949](https://github.com/kruddage/engine/issues/949) is where the editor stops
being DOM and starts being the engine. Four decisions carry it, and each one is
the answer to a question the issue asked out loud.

### Who owns the canvas: nobody in React, in the sense that matters

The element is rendered by `src/viewport/viewport.tsx` and it is **never**
conditionally rendered, keyed, moved between parents or unmounted. The panel is
registered `hideable: false, movable: false` for exactly that reason, and
everything that varies — the bar, the pointer surface, the unbuilt notice, the
error — is a *sibling* of the canvas rather than a wrapper around it.

That is the whole mechanism, and it is deliberately not a portal, a ref cache or
a manual `appendChild`. React does not remount a node whose position in the tree
never changes, so the ordinary rules suffice; the elaborate versions exist to
survive a canvas that moves, and the right answer to a canvas that moves is that
it does not.

`useCanvasSize` does the rest: a `ResizeObserver` on the element for dock
resizes, and a re-armed `matchMedia("(resolution: Ndppx)")` for the DPI change
that fires no resize at all — dragging a window between a laptop screen and an
external monitor changes the ratio while the CSS box stays identical.

### Who owns input: the editor, and the mode says so

> **In editor mode the editor owns the pointer over the canvas. In game mode
> kruddgui does.**

Both halves are enforced. `ui/viewport`'s kruddgui overlay stands its
click-to-pick and its aspect sync down whenever editor mode is lit, and
`ui/gizmo`'s overlay draws without reading a pointer at all. So exactly one
thing acts on a press, and which one is a flag rather than a race.

Against the shell: **the shell owns every accelerator with a modifier, the
viewport owns unmodified keys while it has focus.** That line holds by
construction rather than by inspection — every action in `src/shell/commands.ts`
carries a modifier, so the two sets cannot overlap. Ctrl+S is Save even with the
pointer over the scene; G/R/S switch gizmo mode only when it is.

| | |
|---|---|
| Drag | Orbit |
| Middle-drag, or Ctrl/Cmd-drag | Pan |
| Wheel | Zoom |
| Click | Pick, and a miss clears |
| Arrows / Shift+arrows / `+` `-` | Orbit / pan / zoom, from the keyboard |
| `G` `R` `S` | Move, Rotate, Scale |
| `F` / `Home` | Frame the selection / back to the authored camera |
| `Escape` mid-drag | Abort the whole drag |

### The one frame a press is ambiguous, and what it costs

A press on a gizmo handle drags the selection; a press anywhere else orbits.
**Only the engine can tell them apart** — the handles live in world space and
are hit-tested against the same view·projection the renderer draws with, and a
second copy of that arithmetic in TypeScript is the drift #813 warns about.

So the answer takes a flush. `src/viewport/pointer.ts` holds the gesture in a
`pending` phase until it arrives, accumulating whatever the pointer did
meanwhile, then replays it into whichever branch won. One frame — the same bound
#944's Q1 accepted for every other read, and the frame the drag had not moved
in.

`ViewportState.dragSerial` is what makes that possible: a press that grabs
nothing changes no other field, so without a counter there is no way to tell
"not answered yet" from "answered: nothing".

The machine is **pure** — events in, intents out, no React state and no bridge —
so every branch of it is tested in `test/viewport-pointer.test.ts` without a
canvas, a document or a wasm module.

### What is drawn by whom: the engine draws, and it draws everything

Not one pixel of the scene, the selection, the grid, the gizmo or the axis
indicator is DOM.

| | Where | Why |
|---|---|---|
| **Selection** | `render/scene_renderer`'s outline pass, through the frame graph | Silhouette mask then a full-screen edge detect, against the same depth buffer the scene was drawn into. It predates this issue; this issue *enables* it, by lighting editor mode |
| **Gizmo, grid, axis indicator** | `ui/gizmo`'s kruddgui overlay | #944's Q4: kruddgui is the game's GUI, it draws in the canvas, and the editor's gizmos are its second tenant |
| **Mode buttons, snap field, play switch** | DOM, floated over the canvas | Text and inputs, which the DOM does accessibly for free and immediate-mode does badly |

An SVG overlay projected from a view·projection read across the boundary was the
alternative, and it loses on the first ground rather than the second: it is
always one frame stale, so a gizmo would swim behind the mesh it is attached to
during every orbit. That it would also duplicate the camera's arithmetic is the
second reason, not the first.

**Picking and rendering agree** because the hit-test is generated through the
path that draws: `viewport_pick_entity` raycasts against `mesh_script_generate`
geometry with the entity's own parameter override, and the rotation rings are
hit-tested against the very polyline `draw_ring` emits. #813's finding, applied
twice.

### One view, not four

#949 asks whether the deck holds one view or several and asks for the answer to
be stated. **It holds one.**

Worldcraft's four-pane arrangement is the reference and it is not free: four
cameras, four picks and four gizmo hit-tests a frame, each needing its own
viewport size across a boundary designed around there being one. The engine
draws through a single scene camera today (`scene_renderer`'s `g_cam`), so a
second pane is a renderer change rather than an editor one — and one nobody has
asked for. When somebody does, it arrives as its own issue with its own case,
not as a shape this panel was built around on the chance.

### A drag is one undo step

The gesture opens at `pointerdown`, **before** the engine has said whether a
handle was grabbed. That ordering is the whole mechanism: a gesture opened only
on confirmation would leave the first move outside it, as a history entry of its
own, and undoing a drag would take two presses.

Each move lands on the engine's `set_transform` — the same call an inspector
slider makes — so it coalesces by key inside `world/edit`'s ring exactly as one.
A gesture that edited nothing aborts rather than commits, so an orbit does not
leave a "Transform" entry that undoes nothing.

### Play and edit

`body.editor-mode` was the entire handover in the shipped shell, and its spirit
is kept: **one flag, one command**, and the engine does everything that follows —
pauses the simulation, lights the selection outline, hands the pointer back to
kruddgui. Nothing on this side is torn down or rebuilt, which is what makes
entering and leaving repeatedly cost nothing and need no reload. The canvas never
moves; only what the engine draws into it does.

### Deliberately not done

- **Marquee select.** The issue offers it "if it is cheap" and it is not: a
  rubber band is a frustum query, the engine has a ray query, and the selection
  model is one id (`SelectionState` carries `id`, singular). Multi-select is a
  boundary change before it is a viewport one, and it belongs with whatever adds
  it — not smuggled in as a fourth meaning of a drag.
- **Local-space and screen-space gizmo orientation.** The handles are the
  parent's axes, which for a root entity are the world's. A local/world toggle is
  a button and a flag on `gizmo_frame`; it is left out because nothing has asked
  for it and every mode added untested is a mode that is wrong.

### The boundary work this needed

The shell needed none, and said so. The viewport needed a fourth domain, and it
is the first thing in this initiative to push a change back down into the C
tree:

- `ui/gizmo/` — a new module. `gizmo.c` is pure arithmetic (projection,
  hit-testing, three drag solves) with a native test that drives all of it
  against a synthetic camera; `gizmo_plugin.c` is the vtable and the kruddgui
  overlay.
- `ui/viewport/` — publishes a `viewport_api` (report the size, pick, measure an
  entity) and stands its own click-to-pick down in editor mode.
- `camera_api` — gains `frame()`, because frame-selection needs `fov_y` and the
  eye→target distance and both are the camera's own.
- `ui/bridge/` — the viewport domain, twelve opcodes and one query. **Wire
  version 2**: the reply's `generations` object grew a key.

Nothing else changed shape. Every command lands on a service vtable that already
existed, so a gizmo drag is indistinguishable from a C-side edit and gets the
same undo entry.

The status strip's renderer badge and the console's scrollback still come from
push channels that predate the boundary; fps and the resolution are still
measured from the page, and `src/engine/frame-stats.ts` explains why that is
honest rather than a stand-in.

## The outliner, and what it cost the engine

[#950](https://github.com/kruddage/engine/issues/950) is the smallest issue in
the initiative and the one that pushed the most change down into the C tree. The
reason is worth stating up front: **an outliner is not a view of a list, it is a
second view of the document**, and three of the things it does — select several
entities, move one under another, hide one — had no representation on the far
side of the boundary at all.

### The selection is a set, and the set lives in the engine

`SelectionState` carried one id. #949 said so and said that multi-select "is a
boundary change before it is a viewport one, and it belongs with whatever adds
it". This is whatever adds it.

`struct world` grew a `selected_set` flag column beside the `selected` id, and
`selected` kept its meaning exactly: **the primary member — the one entity a
tool that acts on one entity acts on.** The gizmo, Frame Selection, the
inspector and a game reading the picked piece all still read it and none of them
changed. What changed is that the outline pass now asks `is_selected` per entity
rather than comparing against the primary, so all four of a reader's ctrl-clicked
rows get a ring.

The alternative was a selection set in TypeScript, and it fails the first time
anything else looks: a C-side gizmo, a script, an undo. Two views that can
disagree about what is selected is the failure #947 exists to prevent, and the
outliner is the panel that would have introduced it.

### Ids are stable; the columns are no longer topological

`world.h` used to promise that a parent's index was always below its child's,
and every hierarchy walk in `entity.c` was one forward sweep on the strength of
it. Reparenting ends that promise, and it could not have done anything else:
moving an entity under one that happens to sit at a higher index can preserve
the ordering **or** the ids, and not both.

**Ids won.** The selection, the undo snapshots, the outliner's rows and the
bridge all name entities by id, and an id that moved under a drag would silently
mean a different entity in all four.

What that cost, in full:

| | Was | Is |
|---|---|---|
| `world_propagate_transforms` | one forward sweep | each entity resolved after its ancestors, by climbing to the first resolved one. Still linear in the live count |
| `propagate_subtree`, the destroy cascade | one forward sweep | sweep to a fixed point — as many passes as the subtree is deep |
| `world_export_scene` | indices in id order | indices assigned parent-before-child |

That last row is the one that matters and is easy to miss: `struct scene`'s
ordering (`scene.h`) is a **file format** guarantee, not an implementation
detail — a decoder resolves parents in one pass and would read a forward
reference as a parent that does not exist yet. So the ordering is re-established
at the one place it is actually promised, and `scene_save.c` needed no change at
all because it already recursed from the roots.

Cycles were the failure the old ordering made unrepresentable. `world_set_parent`
is now what makes them so: it refuses a parent that is a descendant of the entity
being moved, and every walk is depth-capped besides, so a corrupt column degrades
rather than spinning.

**A reparent keeps the entity's local transform**, so it is a pure hierarchy edit
that inverts exactly. Preserving the world pose was the alternative and it needs
a divide by the new parent's scale, which is undefined for the zero-scale
entities scenes legitimately contain.

### Hidden and locked: engine state, editor memory, not the document

The issue calls these "editor-side state that does not belong in the document"
and asks where they live and whether they survive a save. Both halves are true
at once, and pretending otherwise is what makes this worth a section.

**They reach the engine, because the engine is what draws and what picks.** An
editor-side "hidden" would dim a row in a list while the entity carried on
rendering — that is not hiding it. So `struct world` grew a `flags` column;
hidden is skipped by the forward pass, the shadow pass and picking, and locked is
skipped by picking alone (a locked entity is a visible backdrop that a click
cannot land on, which is what locking is *for*).

**They are not document state, and the engine treats them accordingly:** never
exported, never captured in an undo snapshot, never on the history ring, and
cleared when a scene is ingested.

- **Does a save carry them? No.** A level that shipped with something invisible
  nobody could find is the failure that decides it.
- **Does Undo un-hide? No.** The snapshot does not carry them, so pressing Undo
  after hiding a light undoes the edit before it. How someone is looking at a
  scene is not a change to the scene.
- **Do they survive a reload? Yes**, and that is the editor's doing:
  `src/panels/outliner/view-state.ts` mirrors them into browser storage beside
  the dock layout and replays them onto a scene once, per document.

The mirror is memory and never truth. A row renders from `scene.tree`'s answer,
exactly as its highlight renders from the engine's selection.

### Expansion state, and the one line that bounds it

Expansion belongs to `react-arborist`'s own store. It survives a selection change
for free, because nothing remounts the tree on one — #950's criterion. It does
**not** survive a scene load, and that takes one line rather than none: the panel
keys the tree on `DocumentState.loads`, a counter the document bumps when a load
lands. Restoring "row 12 was open" onto a scene whose id 12 is something else is
the failure that rules out persisting it.

`loads` is a new field and deliberately not a generation: the scene's generation
is the engine's and moves for every edit. This counts one specific event — **the
world was replaced, so every id now means something else** — and nothing else
says that.

### One selection, two owners, and the deviation

`react-arborist` keeps a selection of its own. Its keyboard navigation, its ARIA
and its drag all read it, and its click handling is already ctrl-to-toggle and
shift-for-a-range — the conventions the issue asks for. Turning it off would mean
hand-rolling the behaviour the dependency exists to provide.

So the split is: **the library decides what the gesture meant; the engine decides
what is selected.** `onSelect` becomes `selection.set` / `.add` / `.clear`
commands, and an effect pushes the engine's answer back into the library whenever
the two differ — in that direction only. Nothing reads the library's copy to
decide what is selected; the rows highlight from `useSelection()`.

Two echoes have to be swallowed for that to terminate, and both are guarded and
tested: `setSelection` calls `onSelect` back synchronously with the *previous*
selection, and the library deselects everything on mount when given no
`selection` prop. Unguarded, opening a dock with something already selected would
clear it.

### What is one undo step

A drag is one, however many rows moved: `onMove` opens a gesture, dispatches one
`entity.parent` per row and commits, and the engine's ring folds them together —
the same mechanism a gizmo drag uses. Delete and duplicate of a multi-selection
are the same shape. Rows whose parent is also in the gesture are dropped first,
because reparenting a child that is already travelling with its parent would
flatten the hierarchy the reader was trying to move intact.

**Deleting a parent deletes its children.** That is what the engine's destroy has
always done — it cascades — and the criterion is to say which, not to change it.

### Wire version 3

The reply's `selection` value grew an `ids` array and the tape grew five
opcodes: `SELECT_ADD`, `SELECT_REMOVE`, `SELECT_CLEAR`, `ENTITY_PARENT`,
`ENTITY_DUPLICATE` and `ENTITY_FLAGS`. The bump is required rather than polite —
a client speaking 2 would highlight one row out of four.

A refused drop comes back as a `command.rejected` event rather than as a failed
batch: dropping a node onto its own descendant is something a reader does by
accident with the mouse, and it must not discard every other command in the
frame. The editor greys the drop cursor out as well, so the refusal arrives
before they let go rather than after.

### Deliberately not done

- **Search by type.** The issue offers it "if it is cheap" and it is not: the
  mask would have to be decoded into words nobody has agreed on. Name and id are
  in, and an unnamed entity renders as `Entity 12` so the id is a string a reader
  can actually see and type.
- **A row count assertion in Vitest.** How many rows a 5,000-entity tree keeps in
  the DOM is a virtualization question, jsdom reports every element as
  zero-sized, and a component test that claimed it would be lying. It is
  Playwright's, and until it is written the honest statement is that this has
  been driven at four rows and inherits `react-arborist`'s bound rather than
  proving its own.

## The inspector, and the form nobody wrote

[#951](https://github.com/kruddage/engine/issues/951) is the smallest panel in
the initiative and it carries the idea with the longest reach: **adding a
parameter to an asset must not require editor code.** The engine's asset scripts
already declare their editing metadata —

```scheme
(edit color)
(edit range 0 1)
```

— GLSL and WGSL emission both ignore that clause entirely, and that is the point
of it. It exists so an inspector can derive its UI from the asset. This panel is
where that either became true or quietly did not.

### The constraint is what the function is given

`src/panels/inspector/controls.ts` is a pure function of `(edit, type,
components)`. It is handed no name, no asset path and no slot, which means it
**cannot** special-case a parameter — the property is enforced by the signature
rather than by a reviewer noticing a `switch`. `param-control.tsx` is the same:
it takes a `ControlSpec` and a value and renders, and it has no idea what an
asset is.

The authored hint is consulted before the type, so a `vec3` tagged `(edit
color)` is a colour well rather than three number boxes; a file that asked the
type first would silently ignore every hint on a multi-component field. A field
with **no** hint still gets a number entry, because most authored params have
none and rendering nothing would make them invisible.

`test/inspector.test.tsx` closes the loop: a parameter named `wobbliness`
appears in no source file in this package — grep for it and the suite is the
only hit — and it has to become a working, labelled, bounded control with no
code written for it. It is asserted for a single entity and again across a
multi-selection, because "zero editor code" holding for one entity and not for
two would be the same failure arriving later.

### A selection of several, merged by declaration

The tempting reading of "shared parameter" is same slot, same field index, and
it is wrong. Two entities can carry different meshes that both declare `width`
at different offsets, and the boundary addresses a write **by index** — so
merging by index edits `width` on one entity and `segments` on the other.

`merge.ts` matches on the declaration instead: name, type, component count, and
the authored hint with its bounds. Each entity keeps its own `(slot, field)`
pair and an edit fans out to all of them inside one gesture, so setting a shared
parameter across four entities is one undo step. Two entities whose `width` is
bounded `0..1` on one asset and `0..10` on the other are deliberately *not*
shared: one slider cannot be honest about both, and showing the primary's bounds
would clamp the other's value to a range its asset never asked for.

Everything the merge drops is counted and said out loud — a parameter only some
of the selection declares, a slot only one of them carries, an entity the engine
has not answered for yet. A panel that silently showed the intersection would
read as an asset with fewer parameters than it has, which is the same failure
the `truncated` warning already exists to prevent.

### Every slider has a number beside it

A range input cannot render a value outside its bounds — the thumb sits at the
bound whatever you do. A slider alone would therefore report an older project's
`5` for a `0..1` parameter as `1`, which is the engine's answer replaced by a
plausible one. So a bounded parameter gets both controls over one value: the
slider for the gesture, the entry for the truth. The entry is the labelled one
and the slider is `aria-hidden`, because they are one control and a screen
reader announcing this parameter twice is worse than either.

It is also the only place an out-of-range value can be *typed*, and every
refusal is a sentence tied to its input by `aria-describedby` rather than a red
border: "not a number", "outside 0–1, clamped", "rounded to a whole number". A
colour is not a reason, and it is invisible to a reader using a screen reader or
one of the several common forms of colour blindness.

### Drag-to-scrub, and where it stops

A press on an **unfocused** number field that travels three pixels becomes a
drag on the value. Focused is left alone deliberately: a field being typed in is
a text field, and taking away select-by-dragging inside it costs more than the
scrub gains. A press that never travels focuses the field, so click-to-type
still works. Touch is excluded entirely — a scrub and a scroll begin with the
same gesture, and a panel the reader cannot scroll is the worse trade.

### Deliberately not done

- **Boolean, string, enum and asset-reference controls.** The issue asks for
  "whatever the metadata vocabulary can express", and the vocabulary is
  `none`, `color` and `range` over `float`, `int` and `vec2`..`vec4` —
  `script-field-edit` in `krudd/engine/world/entity/entity_script.scm` is the
  whole of it. Every one of those is rendered. Adding a `(edit bool)` is an
  engine change first, and the control that follows it is a `controlFor` case
  and nothing else.
- **Texture parameters.** The texture an entity draws with is named inside its
  material's wire form rather than by a ref on the entity, and that form varies
  by material kind. The bridge's shape carries a slot name, so a fourth slot is
  an addition rather than a rework.

## The end of zero-dep

`pnpm-lock.yaml` used to be 23 lines. Its only entry was a `workspace:*` link,
`pnpm install` downloaded nothing, and that was a real position — consistent
with vendored s7, a hand-written CDP client and a build system written in
Scheme.

**This package ended it, on purpose.** A dock splitter, a virtualized tree, a
node canvas and a gesture layer are each months of work the ecosystem has
already done, and hand-rolling them buys nothing an editor's users will ever
see. The rule going forward is #944's: reach for the package, write down what it
is for, move on.

Nothing else changed. The engine still builds through kruddmake with a compiler
and nothing else, `version.txt` is still the only source of the version, and no
package that feeds the C build acquired a registry dependency.

### Every dependency, and what replacing it would cost

The rule is #944's: each entry arrives with the issue that uses it, and each
carries a line saying what replacing it would cost.

`@xyflow/react` is still absent for that reason — it belongs to the node
canvas, and adding it early would mean a dependency whose justification nobody
could check against real code. `react-arborist` arrived with #950, which is the
rule working as intended.

**`@use-gesture/react` is absent for a different reason, and it is a
deviation worth stating.** #949 names it for the camera, on the strength of
#946 having reserved it. It was added, written against, and taken back out,
because the viewport's gesture layer is not the shape it fits:

- The load-bearing part is the `pending` phase — a press whose meaning arrives a
  frame later, from the engine, and is then replayed. No gesture library models
  that, so it would sit on top of one rather than being served by it.
- What is left underneath is `pointerdown`/`move`/`up` with
  `setPointerCapture`, which the DOM does correctly and which the library also
  just calls.
- The one real gain was trackpad pinch, and that is five lines: every browser
  reports a pinch as a wheel event with `ctrlKey` set, and `wheelNotches` gives
  it its own scale.

So the package would have been a dependency whose justification nobody could
check against real code — which is the rule, applied to the package the rule was
written about.

| Package | What it does here | Replacing it |
|---|---|---|
| `react` / `react-dom` | The UI runtime. The substrate decision from #944, not a preference | A rewrite of the editor |
| `vite` | Dev server with fast refresh, and the production build | Weeks, and worse. This is the "not the place to be interesting" choice |
| `@vitejs/plugin-react` | JSX transform and fast refresh for Vite | Nothing else does this job for this bundler |
| `typescript` | Strict mode, and the build fails on a type error | The `.ts` in "the editor is a TypeScript application" |
| `vitest` | The component suite (#944, Q6) | A day to move to `node:test` + a DOM shim, and the shim is what Q6 rejected |
| `jsdom` | The DOM Vitest runs components against | Interchangeable with `happy-dom`; a config line |
| `@testing-library/react` + `@testing-library/dom` | Rendering and querying components in tests | Hand-rolled render helpers — a few hundred lines that drift |
| `react-resizable-panels` | The dock splitters: keyboard-resizable separators with the right ARIA, pointer capture that survives leaving the window, collapse/expand, and constraint solving across nested groups | A week of pointer maths and a permanent source of off-by-a-pixel bugs, for a control nobody will ever compliment |
| `@radix-ui/react-menubar` | The command bar: roving focus across the bar, arrow keys and type-ahead within a menu, Escape, focus restoration, `aria-expanded`, and submenus that flip rather than clip at the screen edge | A month, and a keyboard-accessible menu implementation to maintain forever. The menu *content* is entirely ours — Radix contributes behaviour, not vocabulary |
| `react-arborist` | The outliner's tree: virtualization, drag-and-drop with a drop cursor, keyboard navigation with type-ahead, and the ctrl/shift selection conventions a designer already has in their fingers | A hand-rolled tree, which is fine at 50 rows and unusable at 5,000 — #892's zoo level is the 5,000 case. The selection *conventions* are the part that surprises: they are a week of edge cases, and the library already had them |
| `@radix-ui/react-tabs` | The dock tab strips and the inspector's tab group: the `tablist`/`tab`/`tabpanel` roles wired to each other, and arrow-key navigation | A hand-rolled tab strip, which is the one a screen reader cannot use |
| `@playwright/test` | The browser suite: real Chromium, real WASM | The vendored CDP client plus fixtures, retries, tracing and parallel workers. Considered and rejected in the Q6 decision — that is maintaining a browser-automation framework as a side effect of building an editor |
| `@types/node`, `@types/react`, `@types/react-dom` | Types for the above | Nothing; they are types |
| `@kruddage/engine` | Where the WASM artifact is. A `workspace:*` link, and a **dev** dependency — it is consulted at build time and none of it is bundled | It is the boundary; there is nothing to replace it with |

## How the editor meets the engine

**It consumes the artifact. It never produces one.** kruddmake stays the only
thing that compiles C, and `version.txt` stays the only source of the version —
routing either through Node would put the JS toolchain in the path of a fact the
C build needs (see the root `package.json`'s `//version` note).

`build/engine-artifacts.mts` is the whole of the seam. It asks
`@kruddage/engine` where the outputs are rather than knowing, which is what the
workspace boundary check enforces and the reason that package exists at all. It
does three things:

- **dev** — serves the artifacts under `/engine/`, so `pnpm dev` runs against a
  real engine with fast refresh over the top
- **build** — copies the same files into `dist/engine/`, so what ships is what
  was tested
- **both** — exposes the manifest to the app as `virtual:krudd-engine`, so the
  editor can render the engine's version without a fetch and without a second
  copy of the artifact contract

### The cache-busting stem, deliberately absent

The deployed site renames `index.wasm` to `index.<hash>.wasm`, and the generated
shell finds it only because the same hash was baked into a `Module.locateFile`
hook at build time. Two derivations of one hash in two languages that have to
agree — `@kruddage/site` cross-checks them for exactly that reason.

**The editor sidesteps the mechanism entirely**: it ships the artifacts under
their real names, so its `locateFile` is the identity. That is not an oversight,
and it must not be "fixed" by copying the shell's version — a stem applied to
files that were never renamed asks the browser for a URL that was never staged,
and it fails at runtime and nowhere else. `test/boot.test.ts` guards it.

### An unbuilt engine is a supported state

`pnpm --filter @kruddage/engine run build` needs emsdk, and a contributor
working on the editor's chrome may not have it. Rather than failing, the plugin
reports `built: false` and the app says so on screen with the command that fixes
it. The component suite does not care either way.

What does **not** happen is pretending: `scripts/e2e.mjs` refuses to run the
browser suite against an engine that is not there, rather than skipping quietly.
A skipped browser suite reports green, and a green suite that never booted a
module is precisely what #946 warned about.

## Where the page comes from

**Beside the existing shell, at its own route — not replacing it.**

The generated shell is stamped by the C build: the version, the cache-busting
hook and the Scheme-derived chrome all arrive from kruddmake. Replacing it would
mean moving those into Vite, which is the coupling the root `package.json`
already forbids. Mounting into it would mean the React app inheriting a DOM
another system builds at boot.

Sitting beside it costs nothing, keeps the existing shell working while the
editor is a skeleton, and is what lets
[#953](https://github.com/kruddage/engine/issues/953) retire the old chrome as a
deliberate step rather than as this PR's side effect. `base` is `"./"` so the
same output works at `/editor/` on the deployed site, at `/` under `vite
preview`, and under a preview deploy's PR-number prefix, without a second build.

## How this is tested

[#944's Q6](https://github.com/kruddage/engine/issues/944) decided it: **Vitest
for components, Playwright for the browser.** Both are set up here and each is
proven with real tests, because a configured-but-unused runner decides nothing.

| | What runs there | Where in CI |
|---|---|---|
| **Vitest** (`pnpm test`) | The layout model, the command table, the shell rendered, the boot path up to the module itself, the build plugin's path handling, and the stylesheet's own text | The `workspace` job — no emsdk, no browser |
| **Playwright** (`pnpm test:e2e`) | The engine actually booting: WASM served as `application/wasm`, `main()` running, a live renderer reported | The `build` job, after the engine is built |

**The rule: a test goes in Playwright when it would lie in Vitest, and not
because it feels more real.** A browser suite that grows to cover what jsdom
could have answered is how a fast suite becomes one nobody runs.

`pnpm test` deliberately does not invoke Playwright. The root `pnpm test` runs
in a job with no emsdk, so there is no WASM for a browser to boot; wiring them
together would make the fast suite depend on a toolchain it does not need.

### Two configs, two typechecks

`tsconfig.json` is the application — `src/` and `test/`, and **no Node types**,
so a stray `node:fs` in browser code is a type error rather than something Vite
discovers at bundle time. `tsconfig.node.json` is the build-time half: the Vite
plugin, the scripts and the three configs. `pnpm typecheck` runs both, and
`build` runs `typecheck` — Vite transpiles without checking and would otherwise
ship a type error happily.

## What is deliberately not here

- **Panel contents** — the asset browser and the build panel (#952). Each is a
  placeholder that says so on screen. The viewport (#949), the inspector (#951)
  and the outliner (#950) are real
- **Marquee select** — the issue offers it "if it is cheap" and it is not; see
  the viewport section above for why it is a boundary change first
- **The node graph** — #944's Q5 puts it here, in React, as one canvas. It
  arrives after the panels
- **A phone layout** — ruled out of #944 explicitly. Container queries and
  mount-not-duplicate went in anyway, because they are cheap insurance rather
  than a commitment
