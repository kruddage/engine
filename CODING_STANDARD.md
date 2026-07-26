# Coding Standard

## Philosophy

This engine is built by many hands and many agents, most of which never meet.
The code is the only thing they share, so the code has to carry the
coordination: anyone should be able to open any file and find it shaped the way
they already expect.

From that, one operating rule: **prefer uniformity over individual taste.** When
two forms are both correct, the more canonical one wins — it is the one every
contributor already knows, so it is the one that costs no one a judgment call.
The standards here exist to remove decisions, not to record preferences.

The strongest form of that rule is to take each tool's defaults and not argue.
`rustfmt` and `clippy` at their defaults; Biome at its defaults. A style that is
mechanically produced is a style nobody has to hold an opinion about, and a
formatting diff in review is a review that is not happening.

---

## Rust

`rustfmt` and `clippy` decide, at their defaults, and CI runs both:
`cargo fmt --all --check` and `cargo clippy --workspace --all-targets --
--deny warnings`. There is no `rustfmt.toml` and no `clippy.toml`, on purpose —
adding one is adding a preference.

What the tools cannot decide:

**Public surface is a decision.** Every crate sets `missing_docs = "warn"`, so
`pub` costs a doc comment. Anything that does not need to be public is not:
prefer `pub(crate)`, and keep the exported surface of a crate small enough to
read in one sitting. This is the API-leak defence [#812] asked for, and it is
enforced by the compiler rather than by review.

**Comments explain why, not what.** Don't narrate the code. Do write down the
invariant, the platform quirk, the bug that motivated the shape. A comment that
says what the next line does is noise; a comment that says why the generation
is bumped on `free` rather than on the next `alloc` is the reason the next
person does not "simplify" it.

**Names.** `snake_case` for functions and variables, `UpperCamelCase` for
types, `SCREAMING_SNAKE_CASE` for constants — Rust's own convention, which
clippy already enforces. Name functions as verb phrases.

**Fixed-width types across the boundary.** Anything that crosses into
TypeScript or onto the GPU uses `u32`, `f32`, `i32` and friends and is
`#[repr(C)]`. The layout is part of the contract, not an implementation
detail — the whole point of the boundary is that the other side reads the
bytes directly.

**Tests live beside the code**, in a `#[cfg(test)] mod tests`. Name a test
after the property it holds, not the function it calls:
`a_reused_slot_does_not_answer_to_the_old_handle`, not `test_alloc`. The name
is what a failure report shows, so it should say what broke.

**Test parity is the norm.** The old tree held it — `fg_test.c` at 686 lines
against 623 of implementation — and that is worth re-establishing deliberately
rather than rediscovering. Anything with a rule in it gets a test that fails
when the rule is broken, including the rules about hostile input.

---

## TypeScript

Biome decides, at its defaults (tabs, 80 columns), and CI runs `biome ci .`
for format, lint and import order in one pass. `tsc` runs separately with
every strict flag on and `noEmit` — esbuild is the only thing that emits.

What the tools cannot decide:

**Names.** `camelCase` for functions and variables, `PascalCase` for types and
classes, `SCREAMING_SNAKE_CASE` for module-level constants. This is not C's
`snake_case` and not Rust's — each language keeps its own convention, and the
boundary is where they meet: the generated glue exposes Rust's `set_velocity`,
and `@krudd/boundary` is the one place that renames it to `setVelocity`.

**Exports are declared, not discovered.** A package's `exports` map in its
`package.json` is its public surface; there is no barrel file and no deep
importing. `@krudd/shell-web` exports nothing at all, because it is a shell.

**Private fields use `#`,** not a leading underscore and not `private`. `#`
is enforced at runtime; the other two are conventions a cast can defeat.

**`unknown`, never `any`.** A `catch` binding is `unknown` and gets narrowed
before use.

---

## Both

**An instrument that cannot fail loudly is worse than none.** A capture path,
a test oracle, a status badge — none of them may return the same result for
"worked" and "did nothing". Headless Chrome composites WebGPU canvases as
blank while reporting a live device, and WebGL captures fine headless, so the
obvious sanity check confirms a broken tool. That is the failure mode to design
against.

**Build the oracle before the thing it tests.** The old tree could afford test
parity on the frame graph because the null backend recorded every GPU call, so
a renderer could be asserted on without a GPU. Asserting on pixels instead is
what makes render tests expensive and flaky. See [#826] and [#827].

**The boundary is batched, never per-object.** TypeScript reads and writes
typed-array views over wasm linear memory and calls into Rust once per phase.
Before adding anything to the `#[wasm_bindgen]` surface, read
[`docs/boundary.md`](docs/boundary.md) — it has the ownership rules, the test
for whether something may be exported, and the three ways a view goes stale,
two of which are silent. `cargo xtask bench` keeps the numbers honest.

**One build entry point.** `cargo xtask`. A gate that only exists inside a CI
workflow is a gate nobody can run before pushing.

**Respect the tier order.** A crate or package may depend on its own tier or a
lower one, never a higher one, and nothing may depend on a shell.
`cargo xtask tiers` enforces it. See [`docs/architecture.md`](docs/architecture.md).

**Every source file opens with the SPDX header.**

```rust
// SPDX-License-Identifier: GPL-2.0-or-later
```

```ts
// SPDX-License-Identifier: GPL-2.0-or-later
```

[#812]: https://github.com/kruddage/engine/issues/812
[#826]: https://github.com/kruddage/engine/issues/826
[#827]: https://github.com/kruddage/engine/issues/827
