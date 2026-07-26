# The boundary

Everything Rust and TypeScript say to each other goes through one wasm module
and one wrapper package. This document is the contract between them: what may
cross, who owns what, when a view stops being valid, and what the alternative
costs when measured rather than asserted.

Read it before adding anything to the `#[wasm_bindgen]` surface. The rule it
exists to protect is easy to break by accident and the symptom is not an error
— it is a frame rate, blamed on WebAssembly.

## The one rule

**Calls cross the boundary once per phase, never once per object.**

Rust owns the data. TypeScript reads and writes it *in place*, through
typed-array views mapped straight over wasm linear memory, and calls into Rust
only to make something happen: advance the world, build a frame, decode an
asset.

```
                  ┌───────────────────── wasm linear memory ─────────────────┐
  TypeScript      │  positions: [x0 y0 z0 x1 y1 z1 x2 y2 z2 …]               │
      │           └──▲───────────────────────────────────────────────────────┘
      │              │ Float32Array view — no copy, no marshalling
      ├── positions()┘
      └── tick(dt) ──► one call, whole world
```

That shape is not an optimisation, it is the reason the split works at all.
Rust is the half without a garbage collector; the value of that evaporates if
every entity costs a boundary crossing and a JS allocation to look at.

### Why the storage is struct-of-arrays

A column per field is what makes the view *be* the memory. An array of structs
would force TypeScript to stride over fields it does not want and would make
the view's element type a lie. This is why `crates/world/storage` owns slots
and generations but no component data — see [`architecture.md`](architecture.md).

## What it costs to be wrong

`cargo xtask bench` reads and writes a world of 20 000 entities both ways and
prints the difference. It always builds `--release`, because the batched path
is a walk over a typed array and does not care how Rust was compiled while the
per-call path is Rust code and cares enormously — measured against a debug
build the same two ratios come out three times and eight times larger, which
would be a flattering number for an engine nobody ships. On the machine this
was written on:

| path | ns/entity | vs batched |
|---|---:|---:|
| read · batched view | 7.0 | 1.0× |
| read · one call per entity | 250.2 | **36×** |
| write · into the view | 7.6 | 1.0× |
| write · one call per entity | 40.5 | **5.3×** |

The absolute numbers matter as much as the ratios. At 250 ns per entity,
reading 20 000 positions costs 5 ms — a third of a 60 Hz frame, spent before
anything has been drawn. The same read through the view costs 0.14 ms.

The two rows also differ from each other in a way worth internalising, because
it tells you which crossings are expensive and why:

- **A crossing that returns a heap value is ~36× the batched cost.** The
  per-call read is `Engine::position_of`, a perfectly ordinary-looking
  accessor, and each call pays for a boundary crossing, a Rust allocation for
  the returned `Box<[f32]>`, a copy into a JS `Float32Array`, a Rust free, and
  one more object for the collector to take back.
- **A crossing that only passes scalars is ~5× the batched cost.**
  `Engine::set_position` takes four numbers and returns a `bool`; no allocator
  is involved on either side. Five times is not a cliff, but it is still five
  times, and it is five times *per entity per frame*.

So the rule is not "crossings are catastrophic". It is that a crossing is a
fixed cost with no batch discount, and per-object work is exactly the shape
that multiplies it by the entity count.

`position_of` and `set_position` are exported **only** so the benchmark has
something honest to measure against. Nothing in the engine may call them.

The benchmark fails the build if either ratio falls below 2×, which is far
under what it measures — a tight bound would be a flaky gate rather than a
useful one, and this is a tripwire for the batched path becoming per-object,
not a performance regression gate. It also checksums both paths and fails if
they disagree, because a fast path that quietly skipped the work would
otherwise report a magnificent speedup.

## Booting crosses twice, and both are awaits

The engine is reached through `boot()`, not through `new`. Two things have to
happen before there is a world, and neither can be folded into a constructor:

1. **The module is fetched and instantiated.** wasm-bindgen's `init`.
2. **The GPU adapter and device are requested.** Both are async on the web, so
   `start(canvas)` is an exported `async fn` — a `#[wasm_bindgen(constructor)]`
   cannot return a promise.

`start` takes the canvas *by value* and the surface keeps it, which is what
makes the surface `'static`. A borrowed canvas would put a lifetime on
`Engine`, and a `#[wasm_bindgen]` type cannot carry one.

The canvas has two sizes and only one of them is the renderer's. `canvas.width`
is the drawing buffer, in physical pixels; the CSS width is what the page lays
out. `fitCanvas` in `@krudd/boundary` is the only place that converts between
them, and `World.resize` takes physical pixels so there is nowhere else for a
second `devicePixelRatio` multiply to hide.

The drawing buffer also has a ceiling, and it is lower than a phone. wgpu
validates `Surface::configure` against the device's `max_texture_dimension_2d`,
which the WebGL2 backend pins to that API's floor of 2048
(`MAX_SURFACE_EXTENT`); a portrait Android canvas at `devicePixelRatio` asks
for 1080x2256, and the answer is a validation error that takes the page down,
not a clamp that costs resolution. So `fitCanvas` scales the buffer down to
fit, both sides by one factor so the aspect — and with it the camera's —
survives. The arithmetic is `fit_drawing_buffer` in the wasm module and is
therefore the *same* arithmetic the renderer applies: two implementations that
rounded differently would set the buffer back and forth at each other on every
resize event. `World.viewport` reports what the engine settled on, which is
what the status line shows.

`render()` is a phase call like `tick()`: one crossing for the whole frame,
whatever the draw count. The per-draw data goes into a uniform buffer written
in one upload, for the same reason the position column exists.

## Ownership

| Value | Allocated by | Freed by | Notes |
|---|---|---|---|
| `Engine` | Rust, on `new Engine(…)` | JS, on `free()` or `Symbol.dispose` | wasm-bindgen also registers a `FinalizationRegistry`, so a dropped reference is eventually collected — eventually, at the collector's discretion. The page holds one engine for its lifetime, so this does not come up yet. |
| A column (`positions`, `velocities`) | Rust | Rust | TypeScript never allocates or frees engine data. |
| A `Float32Array` **view** | nobody | nobody | It is a window, not an object with contents. Dropping it frees nothing. |
| A `Float32Array` **returned** from a call | Rust, then copied to JS | the JS collector | This is the per-call path. The copy is the cost. |
| A returned `string` | Rust, then copied to JS | the JS collector | UTF-8 → UTF-16 conversion on every call. Fine for `version()`, not for anything per-frame. |

The short version: **Rust allocates and frees engine data; TypeScript borrows
it.** A pointer that crosses the boundary is a loan, and the term of the loan
is "until the next call that can move it".

## When a view goes stale

Three things invalidate a `Float32Array` over wasm memory. Two of them are
silent.

1. **The column moves.** `spawn` can reallocate the `Vec`, so
   `positions_ptr()` changes and the old view addresses memory that is no
   longer the column. It still reads plausible floats. This is the dangerous
   one.
2. **The column resizes.** `positions_len()` changes and a view built at the
   old length either misses entities or runs past the end.
3. **Linear memory grows.** Growing wasm memory *detaches* the underlying
   `ArrayBuffer`. Every view over it becomes zero-length **in place** — no
   exception, no warning. A detached view reads nothing, which is
   indistinguishable from an engine that stopped simulating.

`@krudd/boundary` is the one place that knows this. Each column caches one
view, compares all three facts — pointer, length, and buffer identity —
against it, and rebuilds when any of them has changed. `World.positions()` and
`World.velocities()` are the same three comparisons over different columns,
which is why they share one `Column` rather than each carrying their own copy
of the rule:

```ts
const view = world.positions();  // cheap when nothing moved: the cached view
world.tick(dt);                  // moves entities, not the column
render(view);                    // still valid — a tick cannot invalidate it
world.spawn(0, 0, 0);            // may reallocate
render(world.positions());       // ask again; never reuse `view` past here
```

**The rule for callers: fetch the view where you use it, and never store one
in a field.** It is a property read plus three comparisons when nothing moved;
it is a correctness bug the day something does.

`packages/base/boundary/harness/memory.test.ts` holds all three cases against
the real module, including a deliberate `memory.grow(1)` — growth cannot be
provoked reliably by allocating, because the allocator reuses freed pages, and
this is a rule worth asserting rather than hoping for.

## A worked example

The whole round trip, from Rust's column to the page and back. Rust owns the
data:

```rust
#[wasm_bindgen]
pub struct Engine {
    positions: Vec<Vec3>,   // #[repr(C)], three f32s, no padding
    // …
}

#[wasm_bindgen]
impl Engine {
    /// One call for the whole world.
    pub fn tick(&mut self, dt: f32) { /* … */ }

    /// The address of the column, for the view to be built over.
    pub fn positions_ptr(&self) -> usize { self.positions.as_ptr() as usize }

    /// Its length in f32s — three per slot.
    pub fn positions_len(&self) -> usize { self.positions.len() * 3 }
}
```

TypeScript reads it without a copy, and writes into it without a call:

```ts
const { world } = await boot({ canvas });
const slot = world.spawn(0, 0, 0);

function frame(dt: number): void {
	world.tick(dt);                     // one crossing, whole world

	const positions = world.positions();  // a view, not a copy
	for (let i = 0; i < positions.length; i += 3) {
		inspect(positions[i], positions[i + 1], positions[i + 2]);
	}

	positions[slot * 3] = 0;            // writes reach Rust's memory directly;
	                                    // the next tick integrates from here

	world.render();                     // one crossing, whole frame
}
```

Three crossings per frame, whatever the entity count: `tick`, and the pointer
and length that `positions()` checks its cached view against. The reads and
the writes themselves cost none — they are loads and stores against memory the
page already has.

### The `#[repr(C)]` part is load-bearing

`Vec3` is `#[repr(C)]` and holds three `f32`s, so `positions_len()` can be
`len() * 3` and the view's indices can be `slot * 3 + component`. Rust's
default layout makes no such promise. Anything crossing the boundary uses
fixed-width types and `#[repr(C)]` — that is in
[`CODING_STANDARD.md`](../CODING_STANDARD.md) and this is what it is for.

## What may be exported

Ask, in order:

1. **Is it per-object?** Then it does not cross. Add a column and a pointer,
   or a batched call that does the whole phase.
2. **Does it return a heap value** — `String`, `Vec<T>`, `Box<[T]>`? Every
   call allocates on both sides. Fine once at boot, never per frame.
3. **Is it a handle?** Good. `Id<K>` in `crates/render/gpu` is a `u32` pair
   precisely so a resource can be named across the boundary without anything
   being copied or kept alive by a JS reference.
4. **Does it need a doc comment explaining when to call it?** Then write one:
   the crate sets `missing_docs`, and the generated `.d.ts` carries the
   comment through to the TypeScript side, where the next person reads it.

## Where the rules live

| Rule | Enforced by |
|---|---|
| The batched path stays the fast one | `cargo xtask bench`, which fails under a 2× floor |
| A stale view is rebuilt, not returned | `cargo xtask test-web`, against the real module |
| The two paths agree about the world | the same tests, and the benchmark's checksums |
| Only `krudd-web` knows wasm-bindgen exists | `cargo xtask tiers`, plus every other crate compiling for the host |
| Nothing reaches around `@krudd/boundary` | the package's `exports` map — a deep import does not resolve |

`cargo xtask check` runs all of them except the benchmark, which CI runs
separately and reports.

## What is not settled

Both halves of this boundary are **hand-written**, and nothing stops them
drifting apart within a single build except wasm-bindgen regenerating the
`.d.ts` every time. Generating both sides from one spec is
[#824](https://github.com/kruddage/engine/issues/824). When it lands, this
document describes what the generator has to produce; the rules do not change.

Handles cross as bare slot indices today, not as generational handles —
`spawn` returns a `u32` that is also the view index. A stale index therefore
addresses whoever was recycled into the slot, where a stale `Handle` inside
Rust resolves to `None`. Widening the boundary to carry the generation is
[#824](https://github.com/kruddage/engine/issues/824)'s business too.

The GC did not go away by choosing TypeScript. It relocated to the host
engine's collector, so the engine ships zero GC bytes and still stutters if
gameplay allocates per entity per frame. Nothing in this document prevents
that; it is a cost the [#812](https://github.com/kruddage/engine/issues/812)
rewrite chose knowingly, and the same one Unity and Godot live with.
