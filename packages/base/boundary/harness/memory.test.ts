// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The memory contract, asserted: a typed-array view over wasm linear memory
 * is only valid until the next thing that can move it.
 *
 * This is the failure the wrapper exists to prevent, and it is worth a test
 * because it does not throw. Growing wasm memory *detaches* the old
 * `ArrayBuffer`; every view over it becomes zero-length in place, silently.
 * A stale view therefore reads nothing and looks exactly like an engine that
 * stopped simulating — an instrument that cannot fail loudly, which
 * `CODING_STANDARD.md` says is worse than none. So the loudness is here
 * instead.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs. See
 * `docs/boundary.md` for the contract these assert.
 */

import assert from "node:assert/strict";
import test from "node:test";
import { load, populate } from "./load";

/** Enough entities that the column has to be reallocated a few times. */
const MANY = 4096;

test("a grown linear memory is noticed and the view rebuilt", async () => {
	const { world, memory } = await load();
	populate(world, 4);
	const before = world.positions();
	assert.equal(before.length, 12, "four slots, three floats each");
	assert.equal(before[0], 1, "entity 0 spawns at x = 1");

	// Grown from here rather than by allocating until Rust grows it: the
	// allocator reuses freed pages, so "spawn a lot" is not a reliable way to
	// force a grow, and this test is about what happens when one occurs.
	memory.grow(1);

	assert.equal(
		before.byteLength,
		0,
		"growing memory should have detached the old buffer — if this ever " +
			"stops being true the cache below is load-bearing for nothing",
	);
	const after = world.positions();
	assert.notEqual(after, before, "the stale view was handed back");
	assert.equal(after.length, 12, "the rebuilt view covers the same column");
	assert.equal(after[0], 1, "the data survived the grow");
});

test("a column that moved is re-viewed at its new address", async () => {
	const { world, engine } = await load();
	populate(world, 1);
	const before = world.positions();
	const address = engine.positions_ptr();

	populate(world, MANY);

	assert.notEqual(
		engine.positions_ptr(),
		address,
		`${MANY} spawns should have reallocated the column`,
	);
	const after = world.positions();
	assert.notEqual(after, before, "the view still points at the old address");
	assert.equal(after.length, (MANY + 1) * 3);
	assert.equal(after[0], 1, "entity 0 kept its position across the move");
});

test("the same view is handed back when nothing moved", async () => {
	const { world } = await load();
	populate(world, 8);

	const first = world.positions();
	assert.equal(world.positions(), first, "an unchanged column re-viewed");

	world.tick(1 / 60);
	assert.equal(
		world.positions(),
		first,
		"a tick moves entities, not the column — no rebuild is warranted",
	);
});

test("a tick is visible through a view already held", async () => {
	const { world } = await load();
	populate(world, 1);
	const view = world.positions();
	const start = view[0] as number;

	world.tick(1);

	assert.ok(
		(view[0] as number) > start,
		"the view is a window onto Rust's memory, not a snapshot of it — " +
			"reading it after a tick must see the tick",
	);
});

test("a write through the view reaches Rust", async () => {
	const { world, engine } = await load();
	const slot = world.spawn(0, 0, 0);
	const view = world.positions();
	view[slot * 3] = 42;
	view[slot * 3 + 1] = 43;
	view[slot * 3 + 2] = 44;

	assert.deepEqual(
		Array.from(engine.position_of(slot) ?? []),
		[42, 43, 44],
		"the boundary is shared memory in both directions, not a read cache",
	);
});

test("the batched and per-call paths report the same world", async () => {
	const { world, engine } = await load();
	populate(world, 64);
	world.tick(0.5);

	const view = world.positions();
	for (let slot = 0; slot < 64; slot++) {
		assert.deepEqual(
			Array.from(engine.position_of(slot) ?? []),
			Array.from(view.subarray(slot * 3, slot * 3 + 3)),
			`slot ${slot} disagrees, so the benchmark compares two things`,
		);
	}
});

test("the velocity column is a view over Rust's memory, not a copy of it", async () => {
	const { world, engine } = await load();
	const slot = world.spawn(0, 0, 0);
	assert.ok(world.setVelocity(slot, 1, 2, 3));

	const view = world.velocities();
	assert.equal(view.length, 3, "one slot, three floats");
	assert.deepEqual(
		Array.from(view),
		[1, 2, 3],
		"the per-call write is visible",
	);

	// And in the other direction: the batched write is what `tick` integrates,
	// which is the whole reason the column is exported.
	view[0] = 10;
	view[1] = -20;
	view[2] = 0;
	world.tick(0.5);
	assert.deepEqual(
		Array.from(engine.position_of(slot) ?? []),
		[5, -10, 0],
		"a velocity written through the view did not reach the integrator",
	);
});

test("the two columns cover the same slots and are indexed alike", async () => {
	const { world } = await load();
	populate(world, 16);

	assert.equal(
		world.velocities().length,
		world.positions().length,
		"a slot in one column and not the other reads another entity's velocity",
	);
	// `populate` spawns on a circle drifting outward, so slot 0 sits at x = 1
	// and moves along +x. Same index into both columns.
	assert.equal(world.positions()[0], 1);
	assert.ok((world.velocities()[0] as number) > 0);
});

test("a velocity view is rebuilt after a spawn moves the column", async () => {
	const { world, engine } = await load();
	populate(world, 1);
	const before = world.velocities();
	const address = engine.velocities_ptr();

	populate(world, MANY);

	assert.notEqual(
		engine.velocities_ptr(),
		address,
		`${MANY} spawns should have reallocated the column`,
	);
	const after = world.velocities();
	assert.notEqual(after, before, "the view still points at the old address");
	assert.equal(after.length, (MANY + 1) * 3);
});

test("a grown linear memory is noticed for the velocity column too", async () => {
	const { world, memory } = await load();
	populate(world, 4);
	const before = world.velocities();
	assert.equal(before.length, 12);
	assert.equal(world.velocities(), before, "an unchanged column re-viewed");

	memory.grow(1);

	assert.equal(
		before.byteLength,
		0,
		"growing memory should have detached the old buffer",
	);
	const after = world.velocities();
	assert.notEqual(after, before, "the stale view was handed back");
	assert.equal(after.length, 12, "the rebuilt view covers the same column");
});

test("the per-call path refuses a dead slot rather than reading a tombstone", async () => {
	const { world, engine } = await load();
	const slot = world.spawn(1, 2, 3);
	assert.ok(world.despawn(slot));

	assert.equal(engine.position_of(slot), undefined);
	assert.equal(engine.set_position(slot, 9, 9, 9), false);
	assert.equal(
		world.positions().length,
		3,
		"the tombstone keeps its slot, so every index the page holds stays valid",
	);
});
