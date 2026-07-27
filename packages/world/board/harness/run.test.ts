// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The interpreter, against a world that is nothing but two columns.
 *
 * The numbers a screenshot cannot assert: where each entity is after a given
 * number of frames, what the paint lane reached, and what an edit did to the
 * next one. The world it runs against is `harness/world.ts`, shared with the
 * suites that run a board for some other reason.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import type { PointerFrame, Project, Registry, Wire } from "../src/index";
import {
	BoardError,
	cloneProject,
	DOCUMENT_VERSION,
	drivesTheGame,
	FRAME_BOARD,
	NO_POINTER,
	PAINT_TO_DRAW,
	ROOT_BOARD,
	Runner,
	setParam,
	TRIANGLES,
	toggleWire,
	validate,
} from "../src/index";
import { at, TestWorld } from "./world";

/** The frame the render test steps at, and the count it steps. */
const DT = 1 / 60;
const FRAMES = 90;

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
	assert.equal(
		world.blanks,
		0,
		"and never a blank one while the wire is whole",
	);
	assert.equal(world.scale, 0.35, "the scale the document asked for");
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
	assert.equal(
		world.blanks,
		1,
		"and the screen has to be left showing nothing, not the last frame",
	);

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

test("cutting the wire the proof of life cuts stops the drawing, live", () => {
	// **The end-to-end shape of PR-7's acceptance criterion**, at the level a
	// unit test can reach it: cut `paint → draw entities` on a running board,
	// and the very next frame draws nothing and blanks the screen. Reconnect,
	// and the triangles are back. The browser half — that the canvas really does
	// go to the clear colour — is `cargo xtask render-test`.
	const world = new TestWorld();
	const project = cloneProject(TRIANGLES);
	const runner = new Runner(project, world);
	runner.start();
	runner.frame(DT);
	assert.equal(world.draws, 1);
	assert.equal(world.blanks, 0);

	assert.ok(
		drivesTheGame(project, project.root, PAINT_TO_DRAW),
		"and it is a wire that genuinely drives the running game, so it is cuttable",
	);
	assert.equal(toggleWire(project, project.root, PAINT_TO_DRAW), true);
	runner.reload();
	runner.frame(DT);

	assert.equal(world.draws, 1, "no reload, and the next frame drew nothing");
	assert.equal(world.blanks, 1, "the screen is left blank rather than frozen");

	assert.equal(toggleWire(project, project.root, PAINT_TO_DRAW), false);
	runner.reload();
	runner.frame(DT);
	assert.equal(world.draws, 2, "reconnect and the triangles are back");
	assert.equal(world.blanks, 1);
});

test("only wires that genuinely drive the running game are cuttable", () => {
	// A control that looks live and changes nothing teaches the wrong thing
	// about what a board is.
	const project = cloneProject(TRIANGLES);
	assert.ok(drivesTheGame(project, project.root, PAINT_TO_DRAW));
	assert.ok(
		!drivesTheGame(project, project.root, "data-recycle-draw"),
		"a data wire is not what the interpreter follows",
	);
	assert.ok(
		!drivesTheGame(project, FRAME_BOARD, "exec-begin-clear"),
		"and the frame board is viewable rather than runnable",
	);
});

test("setting a param edits the document and reaches the next frame", () => {
	const world = new TestWorld();
	const project = cloneProject(TRIANGLES);
	const runner = new Runner(project, world);
	runner.start();
	runner.frame(DT);
	assert.equal(world.scale, 0.35);

	assert.ok(setParam(project, project.root, "draw", "scale", 1.25));
	runner.reload();
	runner.frame(DT);
	assert.equal(world.scale, 1.25, "the value edited is the value that runs");

	assert.ok(
		!setParam(project, project.root, "draw", "nonesuch", 1),
		"a param the kind does not declare is refused rather than added",
	);
	assert.deepEqual(
		validate(project),
		[],
		"and the document still holds together",
	);
});

test("editing the fixture's copy leaves the fixture alone", () => {
	// A module-level constant every other test reads is not a source of truth
	// anyone may edit.
	const project = cloneProject(TRIANGLES);
	toggleWire(project, project.root, PAINT_TO_DRAW);
	const original = TRIANGLES.boards[ROOT_BOARD]?.wires.find(
		(wire) => wire.id === PAINT_TO_DRAW,
	);
	assert.notEqual(original?.cut, true);
});

test("a node reads this frame's pointer sample off RunContext, not off a wire", () => {
	// PR-3's whole point: the graph's first input. `probe` has no input port at
	// all — pointer data reaches a node through `RunContext.pointer`, the same
	// way `integrate` reaches its columns through `c.world`, not through a data
	// wire this interpreter evaluates.
	const seen: PointerFrame[] = [];
	const kinds: Registry = {
		start: {
			title: "Start",
			entry: "start",
			inputs: [],
			outputs: [],
			params: [],
		},
		step: { title: "Step", entry: "step", inputs: [], outputs: [], params: [] },
		paint: {
			title: "Paint",
			entry: "paint",
			inputs: [],
			outputs: [],
			params: [],
		},
		probe: {
			title: "Probe",
			inputs: [],
			outputs: [],
			params: [],
			run: (c) => {
				seen.push(c.pointer);
			},
		},
	};
	const project: Project = {
		version: DOCUMENT_VERSION,
		root: "root",
		boards: {
			root: {
				title: "root",
				nodes: [
					{ id: "start", kind: "start", lane: "start", column: 0 },
					{ id: "step", kind: "step", lane: "step", column: 0 },
					{ id: "probe", kind: "probe", lane: "step", column: 1 },
					{ id: "paint", kind: "paint", lane: "paint", column: 0 },
				],
				wires: [
					{
						id: "exec-step-probe",
						from: { node: "step", port: "out" },
						to: { node: "probe", port: "in" },
						kind: "exec",
					},
				],
			},
		},
	};

	const runner = new Runner(project, new TestWorld(), kinds);
	runner.start();
	assert.equal(
		seen.length,
		0,
		"probe sits in the step lane, not the start one",
	);

	runner.frame(1 / 60);
	assert.deepEqual(
		seen[0],
		NO_POINTER,
		"a caller that hands nothing over gets nothing pressed, nowhere in particular",
	);

	const pressed: PointerFrame = { x: 0.5, y: 0.25, pressed: 1 };
	runner.frame(1 / 60, pressed);
	assert.deepEqual(
		seen[1],
		pressed,
		"and a caller that samples one gets it, whole",
	);
});
