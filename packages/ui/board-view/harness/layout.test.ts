// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Where the board puts things.
 *
 * The geometry is the half of a board view that goes subtly wrong in ways a
 * screenshot does not catch — a wire that lands two pixels off its port in one
 * flow direction and not the other, a lane sized for the tallest node in a
 * different lane, a column that slid left because nothing occupied it. So the
 * arithmetic lives apart from the DOM and is asserted here, against sizes a
 * test chooses rather than sizes a browser measured.
 *
 * The rule the sizes are standing in for: they are *measured*, never guessed.
 * A node box is as tall as its own content, and the ports are fractions of the
 * box — so a guessed height spills the text and lands the wires off their
 * ports in one stroke.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import type { Board } from "@krudd/board";
import { KINDS, ROOT_BOARD, TRIANGLES } from "@krudd/board";
import { flowFor, layout, NARROW_WIDTH, ports, type Size } from "../src/index";

/** The fixture's root board. */
function board(): Board {
	const held = TRIANGLES.boards[ROOT_BOARD];
	assert.ok(held !== undefined);
	return held;
}

/**
 * A size for every node, with one deliberately taller than the rest.
 *
 * Uniform sizes would let a lane sized from the wrong node pass.
 */
function sizes(board: Board): Map<string, Size> {
	return new Map(
		board.nodes.map((node) => [
			node.id,
			{ width: 208, height: node.id === "ring" ? 140 : 72 },
		]),
	);
}

/** Everything laid out, at one flow. */
function placed(flow: "across" | "down") {
	const held = board();
	return layout(held, KINDS, sizes(held), { flow });
}

test("the flow turns over at the narrow width, not at a device", () => {
	assert.equal(flowFor(NARROW_WIDTH), "across");
	assert.equal(flowFor(NARROW_WIDTH - 1), "down");
	assert.equal(flowFor(390), "down", "a phone stacks");
	assert.equal(flowFor(1440), "across", "a desktop runs across");
});

test("lanes run across on a wide board and stack down on a narrow one", () => {
	const across = placed("across");
	const down = placed("down");

	const ring = (l: typeof across) =>
		l.nodes.find((node) => node.id === "ring") as (typeof across.nodes)[number];
	const start = (l: typeof across) =>
		l.nodes.find(
			(node) => node.id === "start",
		) as (typeof across.nodes)[number];

	assert.ok(
		ring(across).x > start(across).x && ring(across).y === start(across).y,
		"across: column 1 sits to the right of column 0, on the same line",
	);
	assert.ok(
		ring(down).y > start(down).y && ring(down).x === start(down).x,
		"down: column 1 sits below column 0, in the same line",
	);
	assert.ok(
		down.height > across.height && down.width < across.width,
		"the same board is taller and narrower when it stacks",
	);
});

test("lanes never overlap, and are as tall as what is in them", () => {
	for (const flow of ["across", "down"] as const) {
		const board = placed(flow);
		for (let i = 1; i < board.lanes.length; i++) {
			const above = board.lanes[i - 1] as (typeof board.lanes)[number];
			const below = board.lanes[i] as (typeof board.lanes)[number];
			assert.ok(
				below.y >= above.y + above.height,
				`${flow}: lane ${below.lane} starts before lane ${above.lane} ends`,
			);
		}
		// The start lane holds the deliberately tall node, so it must be the
		// taller of the two two-node lanes in an across flow.
		const start = board.lanes.find((lane) => lane.lane === "start");
		const paint = board.lanes.find((lane) => lane.lane === "paint");
		assert.ok(start !== undefined && paint !== undefined);
		assert.ok(
			start.height > paint.height,
			`${flow}: a lane sized from the wrong node's height`,
		);
	}
});

test("every node sits inside its own lane", () => {
	for (const flow of ["across", "down"] as const) {
		const result = placed(flow);
		const lanes = new Map(result.lanes.map((lane) => [lane.lane, lane]));
		for (const node of result.nodes) {
			const held = board().nodes.find((n) => n.id === node.id);
			const lane = lanes.get(held?.lane ?? "start");
			assert.ok(lane !== undefined);
			assert.ok(
				node.y >= lane.y && node.y + node.height <= lane.y + lane.height + 1,
				`${flow}: node ${node.id} spills out of the ${lane.lane} lane`,
			);
		}
	}
});

test("a wire lands on its port, on the edge the flow put it on", () => {
	const across = placed("across");
	const exec = across.wires.find((wire) => wire.id === "exec-start-ring");
	assert.ok(exec !== undefined);
	const start = across.nodes.find((node) => node.id === "start");
	const ring = across.nodes.find((node) => node.id === "ring");
	assert.ok(start !== undefined && ring !== undefined);

	assert.equal(exec.from.x, start.x + start.width, "leaves the trailing edge");
	assert.equal(exec.to.x, ring.x, "arrives at the leading edge");
	assert.ok(
		exec.from.y > start.y && exec.from.y < start.y + start.height,
		"and somewhere down the side of the box, not off it",
	);

	const down = placed("down");
	const stacked = down.wires.find((wire) => wire.id === "exec-start-ring");
	const top = down.nodes.find((node) => node.id === "start");
	assert.ok(stacked !== undefined && top !== undefined);
	assert.equal(
		stacked.from.y,
		top.y + top.height,
		"stacked, a wire leaves the bottom instead — the ports re-route with the flow",
	);
});

test("two ports on one node get two different places", () => {
	// `ring` has both a position and a velocity output. Wires that shared an
	// anchor would read as one wire, which is the whole point of drawing ports.
	const across = placed("across");
	const position = across.wires.find((w) => w.id === "data-ring-integrate");
	const velocity = across.wires.find((w) => w.id === "data-ring-velocity");
	assert.ok(position !== undefined && velocity !== undefined);
	assert.notEqual(position.from.y, velocity.from.y);
	assert.equal(position.from.x, velocity.from.x, "same edge, different height");
});

test("the execution port comes first on both sides", () => {
	// So the exec chain runs along one consistent line through a lane rather
	// than wandering up and down with the number of data ports.
	const integrate = board().nodes.find((node) => node.id === "integrate");
	assert.ok(integrate !== undefined);
	assert.deepEqual(ports(KINDS, integrate, "in"), [
		"in",
		"position",
		"velocity",
	]);
	assert.deepEqual(ports(KINDS, integrate, "out"), ["out", "position"]);
});

test("an exec wire lands at the same fraction on both ends, whatever the data ports", () => {
	// `step` has no data ports at all; `integrate` has two on its `in` side.
	// Before EXEC_FRACTION, the exec port's position was spread evenly with
	// whatever data ports rode along beside it, so the two ends landed at
	// different fractions of the node's width and the straight wire between
	// them bowed into a wobble. Down flow stacks every node at the same `x`,
	// so an exec wire between aligned ends must be perfectly vertical.
	const down = placed("down");
	const wire = down.wires.find((w) => w.id === "exec-step-integrate");
	assert.ok(wire !== undefined);
	assert.equal(wire.from.x, wire.to.x, "the exec wire runs straight down");

	// `integrate` (one data port out) and `recycle` (one data port in) also
	// have to agree, and did even before the fix — this guards against a
	// change that fixes `step`/`integrate` but breaks this pair.
	const other = down.wires.find((w) => w.id === "exec-integrate-recycle");
	assert.ok(other !== undefined);
	assert.equal(other.from.x, other.to.x, "also runs straight down");
});

test("every wire in the document is drawn", () => {
	const across = placed("across");
	assert.equal(
		across.wires.length,
		board().wires.length,
		"a wire that silently failed to place is a wire nobody can see is missing",
	);
	assert.deepEqual(
		[...new Set(across.wires.map((wire) => wire.kind))].sort(),
		["data", "exec"],
		"both kinds reach the drawing, and are told apart",
	);
	for (const wire of across.wires) {
		assert.ok(wire.path.startsWith("M "), `wire ${wire.id} has no path`);
	}
});

test("a node nobody measured is dropped rather than placed at a guess", () => {
	const held = board();
	const partial = sizes(held);
	partial.delete("recycle");
	const result = layout(held, KINDS, partial, { flow: "across" });

	assert.ok(
		!result.nodes.some((node) => node.id === "recycle"),
		"placing it would mean inventing a size, which is the failure to avoid",
	);
	assert.ok(
		!result.wires.some((wire) => wire.id === "exec-integrate-recycle"),
		"and its wires go with it rather than pointing at nothing",
	);
});

test("an empty board lays out to nothing rather than to a guess", () => {
	const result = layout({ title: "", nodes: [], wires: [] }, KINDS, new Map(), {
		flow: "across",
	});
	assert.deepEqual(result.lanes, []);
	assert.deepEqual(result.nodes, []);
	assert.ok(result.width > 0 && result.height > 0, "still a legal surface");
});
