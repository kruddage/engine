// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The interpreter, against a world that is nothing but two columns.
 *
 * The point of running these without wasm is that they can say what the
 * *numbers* are. `render-test` proves the document drives the real engine —
 * pixel for pixel, which is the acceptance criterion that matters — but a
 * screenshot cannot tell you that entity three is at x = 0.4 + 0.25 × 1.5,
 * and that is the assertion that catches an interpreter which is wrong by a
 * frame or by a factor of dt.
 *
 * The world here satisfies `WorldView` structurally, exactly as
 * `@krudd/boundary`'s `World` does. It grows its columns on `spawn` and hands
 * back fresh arrays afterwards, which is not a convenience — it is the
 * boundary's own rule, that a `spawn` can move a column and a view taken
 * beforehand is addressing memory that is no longer it. A node that cached
 * its view across a spawn fails here rather than in a browser.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import type { Project, Wire, WorldView } from "../src/index";
import {
	BoardError,
	FRAME_BOARD,
	PAINT_TO_DRAW,
	ROOT_BOARD,
	Runner,
	TRIANGLES,
} from "../src/index";

/** The frame the render test steps at, and the count it steps. */
const DT = 1 / 60;
const FRAMES = 90;

/** A world that is two columns and a draw counter. */
class TestWorld implements WorldView {
	#positions: Float32Array<ArrayBuffer> = new Float32Array(0);
	#velocities: Float32Array<ArrayBuffer> = new Float32Array(0);
	/** How many times the paint lane reached the renderer. */
	draws = 0;

	get slotCount(): number {
		return this.#positions.length / 3;
	}

	spawn(x: number, y: number, z: number): number {
		const slot = this.slotCount;
		// New arrays, not resized ones — the real columns move when they grow,
		// and a node holding a stale view must fail here too.
		this.#positions = grown(this.#positions);
		this.#velocities = grown(this.#velocities);
		this.#positions.set([x, y, z], slot * 3);
		return slot;
	}

	positions(): Float32Array {
		return this.#positions;
	}

	velocities(): Float32Array {
		return this.#velocities;
	}

	render(): void {
		this.draws += 1;
	}
}

/** One more slot's worth of column, carrying what was already there. */
function grown(column: Float32Array<ArrayBuffer>): Float32Array<ArrayBuffer> {
	const next = new Float32Array(column.length + 3);
	next.set(column);
	return next;
}

/** One entity's position. */
function at(world: TestWorld, slot: number): [number, number, number] {
	const p = world.positions();
	return [
		p[slot * 3] as number,
		p[slot * 3 + 1] as number,
		p[slot * 3 + 2] as number,
	];
}

/** The fixture with its root board's wires replaced. */
function rewired(keep: (id: string) => boolean): Project {
	const root = TRIANGLES.boards[ROOT_BOARD];
	assert.ok(root !== undefined);
	return {
		...TRIANGLES,
		boards: {
			...TRIANGLES.boards,
			[ROOT_BOARD]: { ...root, wires: root.wires.filter((w) => keep(w.id)) },
		},
	};
}

test("the lanes run in the order the wires say", () => {
	const runner = new Runner(TRIANGLES, new TestWorld());
	assert.deepEqual(runner.order("start"), ["ring"]);
	assert.deepEqual(runner.order("step"), ["integrate", "recycle"]);
	assert.deepEqual(runner.order("paint"), ["draw"]);
});

test("start spawns the ring the document describes", () => {
	const world = new TestWorld();
	new Runner(TRIANGLES, world).start();

	assert.equal(world.slotCount, 8, "count = 8, from the kind's default");
	// Entity 0 sits at angle 0: x = radius, y = 0. Compared with a tolerance
	// because the column is `f32` — 0.4 reads back as 0.4000000059604645, and
	// asserting on the exact double would be asserting that the engine stores
	// something it does not.
	const [x0, y0, z0] = at(world, 0);
	assert.ok(Math.abs(x0 - 0.4) < 1e-6, `entity 0 is at x = ${x0}`);
	assert.equal(y0, 0);
	assert.equal(z0, 0);
	const velocity = world.velocities();
	assert.ok(
		Math.abs((velocity[0] as number) - 0.25) < 1e-6,
		"drifting outward at speed, along its own ray",
	);
	// Every entity is on the circle, whatever its angle.
	for (let slot = 0; slot < 8; slot++) {
		const [x, y] = at(world, slot);
		assert.ok(
			Math.abs(Math.hypot(x, y) - 0.4) < 1e-6,
			`entity ${slot} is off the spawn circle`,
		);
	}
});

test("a frame moves every entity by its velocity, once", () => {
	const world = new TestWorld();
	const runner = new Runner(TRIANGLES, world);
	runner.start();
	runner.frame(DT);

	assert.ok(
		Math.abs(at(world, 0)[0] - (0.4 + 0.25 * DT)) < 1e-6,
		"one frame of drift, not two and not none",
	);
	assert.equal(runner.frameCount, 1);
	assert.ok(Math.abs(runner.elapsed - DT) < 1e-9);
});

test("the frame the render test screenshots is where the document puts it", () => {
	// The tie between this suite and `cargo xtask render-test`: 90 frames at
	// 1/60 is 1.5 simulated seconds, and the reference image was taken there.
	// If the interpreter were out by a frame or by a factor of dt, this is
	// what would say so in numbers rather than in pixels.
	const world = new TestWorld();
	const runner = new Runner(TRIANGLES, world);
	runner.start();
	for (let i = 0; i < FRAMES; i++) {
		runner.frame(DT);
	}

	const expected = 0.4 + 0.25 * (FRAMES * DT);
	assert.ok(
		Math.abs(at(world, 0)[0] - expected) < 1e-4,
		`entity 0 is at ${at(world, 0)[0]}, not ${expected}`,
	);
	assert.ok(
		expected < 3.2,
		"and nothing has recycled yet, so the frame is fully determined by the count",
	);
	assert.equal(world.draws, FRAMES, "one draw call per frame, no more");
});

test("recycle puts a strayed entity back on the circle along its own ray", () => {
	const world = new TestWorld();
	const runner = new Runner(TRIANGLES, world);
	runner.start();

	// Straight out past the limit, through the view — which is how the page
	// has always edited the world.
	world.positions()[0] = 10;
	world.positions()[1] = 0;
	runner.frame(DT);

	const [x, y] = at(world, 0);
	assert.ok(Math.abs(Math.hypot(x, y) - 0.4) < 1e-5, "back onto the circle");
	assert.ok(x > 0, "and along the ray it left by, not somewhere new");
});

test("cutting the paint wire stops the draw, and reconnecting resumes it", () => {
	// The mechanism PR-7's proof of life rests on: the wires are what drive
	// the running game, not a decoration over something else that does.
	const world = new TestWorld();
	const cut = new Runner(
		rewired((id) => id !== PAINT_TO_DRAW),
		world,
	);
	cut.start();
	cut.frame(DT);

	assert.deepEqual(cut.order("paint"), [], "nothing left to run in the lane");
	assert.equal(world.draws, 0, "a cut wire has to actually stop the drawing");

	const whole = new Runner(TRIANGLES, world);
	whole.frame(DT);
	assert.equal(world.draws, 1);
});

test("a board of kinds that cannot run is refused by name", () => {
	// The frame board is viewable, not runnable — its kinds have no
	// implementation at all. Opening a project on it must say so rather than
	// running an empty chain, because "this board did nothing" and "this board
	// ran" are the two things that may never look alike.
	let error: unknown;
	try {
		new Runner({ ...TRIANGLES, root: FRAME_BOARD }, new TestWorld());
	} catch (thrown) {
		error = thrown;
	}
	assert.ok(error instanceof BoardError, "it should refuse to run at all");
	assert.ok(
		error.message.includes("clear") && error.message.includes("implementation"),
		`the offending node should be named: ${error.message}`,
	);
});

test("a document that does not hold together never starts", () => {
	let error: unknown;
	try {
		new Runner({ ...TRIANGLES, root: "nonesuch" }, new TestWorld());
	} catch (thrown) {
		error = thrown;
	}
	assert.ok(error instanceof BoardError);
	assert.ok(error.message.includes("nonesuch"));
});

test("an edit reaches the next frame after a reload, without a new runner", () => {
	// What makes an edit reach the running game rather than needing a reload:
	// the document is held by reference and the chains are rebuilt on demand.
	const world = new TestWorld();
	const project = rewired(() => true);
	const runner = new Runner(project, world);
	runner.start();
	runner.frame(DT);
	assert.equal(world.draws, 1);

	const root = project.boards[ROOT_BOARD];
	assert.ok(root !== undefined);
	// The edit: cut the wire out of paint. Cast because the document's types
	// are `readonly` — an editor mutates through its own API, and this is
	// standing in for one that does not exist until PR-7.
	(root as unknown as { wires: Wire[] }).wires = root.wires.filter(
		(wire) => wire.id !== PAINT_TO_DRAW,
	);
	runner.reload();
	runner.frame(DT);

	assert.equal(world.draws, 1, "the next frame drew nothing, with no reload");
});
