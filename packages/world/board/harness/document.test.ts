// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The triangles project, and the round trip through text.
 *
 * The fixture is the first krudd project and the thing the proof of life is
 * performed on, so the assertions here are about it being the engine that
 * runs today rather than something that merely parses: the same eight
 * entities, the same spawn radius, the same drift, and the frame one level
 * down.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import {
	DOCUMENT_VERSION,
	FRAME_BOARD,
	KINDS,
	kindOf,
	PAINT_TO_DRAW,
	paramOf,
	parseProject,
	ROOT_BOARD,
	serializeProject,
	TRIANGLES,
	validate,
} from "../src/index";

test("the triangles project holds together", () => {
	assert.deepEqual(
		validate(TRIANGLES).map((p) => p.message),
		[],
		"the fixture every other test is written against must itself be valid",
	);
});

test("a serialised document parses back to exactly what it was", () => {
	const text = serializeProject(TRIANGLES);
	const read = parseProject(text);

	assert.deepEqual(read, TRIANGLES, "the document did not survive the trip");
	assert.equal(
		serializeProject(read),
		text,
		"writing it again produced different bytes — a save would show a diff " +
			"nobody made",
	);
});

test("the project opens on a board it holds, and the frame is one level down", () => {
	assert.equal(TRIANGLES.version, DOCUMENT_VERSION);
	assert.ok(Object.hasOwn(TRIANGLES.boards, ROOT_BOARD));

	const root = TRIANGLES.boards[ROOT_BOARD];
	assert.ok(root !== undefined);
	const draw = root.nodes.find((node) => node.id === "draw");
	assert.equal(
		draw?.board,
		FRAME_BOARD,
		"opening Draw Entities is what shows the frame",
	);

	const frame = TRIANGLES.boards[FRAME_BOARD];
	assert.deepEqual(
		frame?.nodes.map((node) => kindOf(KINDS, node.kind)?.title),
		["Begin Frame", "Clear", "Camera", "Draw", "Present"],
		"the frame reads clear → camera → draw → present",
	);
});

test("the fixture runs on the engine's own constants", () => {
	// These are what packages/shell/web/src/index.ts and crates/shell/web
	// hardcode today. The interpreter reproducing the page pixel for pixel
	// depends on them, so a change here should have to be a deliberate one.
	const root = TRIANGLES.boards[ROOT_BOARD];
	assert.ok(root !== undefined);
	const settings = new Map(
		root.nodes.map((node) => {
			const kind = kindOf(KINDS, node.kind);
			assert.ok(kind !== undefined, `no kind for \`${node.id}\``);
			return [
				node.id,
				Object.fromEntries(
					kind.params.map((p) => [p.name, paramOf(node, kind, p.name)]),
				),
			];
		}),
	);

	assert.deepEqual(settings.get("ring"), {
		count: 8,
		radius: 0.4,
		speed: 0.25,
	});
	assert.deepEqual(settings.get("recycle"), { limit: 3.2, radius: 0.4 });
	assert.deepEqual(settings.get("draw"), { mesh: "triangle", scale: 0.35 });
});

test("world state moves between nodes a column at a time", () => {
	// The load-bearing invariant, asserted where it can be: a graph whose data
	// ports carried one entity's worth of anything would be a graph that
	// executes per object, which is the boundary crossing the engine exists to
	// avoid. `vec3` on a port would be exactly that mistake.
	for (const [name, kind] of Object.entries(KINDS)) {
		for (const port of [...kind.inputs, ...kind.outputs]) {
			assert.notEqual(
				port.type,
				"vec3",
				`\`${name}.${port.name}\` carries one entity's worth of data`,
			);
		}
	}

	const integrate = kindOf(KINDS, "integrate");
	assert.deepEqual(
		integrate?.inputs.map((p) => `${p.name}: ${p.type}`),
		["position: column<vec3>", "velocity: column<vec3>"],
		"integrate takes the columns and walks them",
	);
});

test("a param a node does not set falls back to its kind's default", () => {
	const kind = kindOf(KINDS, "spawn-ring");
	assert.ok(kind !== undefined);
	const bare = {
		id: "n",
		kind: "spawn-ring",
		lane: "start",
		column: 0,
	} as const;

	assert.equal(paramOf(bare, kind, "radius"), 0.4, "the kind's default");
	assert.equal(
		paramOf({ ...bare, params: { radius: 1.5 } }, kind, "radius"),
		1.5,
		"what the node says wins",
	);
	assert.equal(
		paramOf(bare, kind, "nonesuch"),
		undefined,
		"a param the kind does not declare has no value to fall back to",
	);
});

test("the wire the proof of life cuts is in the fixture and drives the game", () => {
	const root = TRIANGLES.boards[ROOT_BOARD];
	const wire = root?.wires.find((w) => w.id === PAINT_TO_DRAW);
	assert.ok(wire !== undefined, "cutting it is PR-7's acceptance criterion");
	assert.equal(wire.kind, "exec");
	assert.deepEqual(wire.from, { node: "paint", port: "out" });
	assert.deepEqual(wire.to, { node: "draw", port: "in" });
});
