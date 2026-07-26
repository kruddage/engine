// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * What a broken document does, and what it says.
 *
 * Every case here is written the same way: take the fixture, break exactly
 * one thing, and assert both that it is refused *and* that the message names
 * the node or the wire at fault. The second half is the point. A validator
 * that returns "invalid" has moved the work of finding the fault onto
 * whoever is holding the board, and `CODING_STANDARD.md`'s rule about
 * instruments that cannot fail loudly applies to error messages as much as to
 * capture paths.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import type { Board, BoardNode, Project, Wire } from "../src/index";
import {
	BoardError,
	DOCUMENT_VERSION,
	describe,
	parseProject,
	ROOT_BOARD,
	serializeProject,
	TRIANGLES,
	validate,
} from "../src/index";

/** The fixture's root board, which every case here starts from. */
function root(): Board {
	const board = TRIANGLES.boards[ROOT_BOARD];
	assert.ok(board !== undefined);
	return board;
}

/** The fixture with its root board's nodes and wires replaced. */
function project(nodes: BoardNode[], wires: Wire[]): Project {
	return {
		...TRIANGLES,
		boards: {
			...TRIANGLES.boards,
			[ROOT_BOARD]: { ...root(), nodes, wires },
		},
	};
}

/** The fixture with one more wire on the root board. */
function withWire(wire: Wire): Project {
	return project([...root().nodes], [...root().wires, wire]);
}

/** Every problem in a project, as the lines a person would read. */
function problems(value: Project): string[] {
	return validate(value).map(describe);
}

/**
 * Asserts some problem names every one of `named`, and returns that line.
 *
 * Not "exactly one problem": breaking one thing on a board often breaks a
 * second thing honestly. Wiring a node that is already run by something makes
 * a ring as well as a second run, and both are worth saying. What matters is
 * that the fault a person is looking for is in there, named.
 */
function names(value: Project, ...named: string[]): string {
	const found = problems(value);
	const line = found.find((problem) =>
		named.every((name) => problem.includes(name)),
	);
	assert.ok(
		line !== undefined,
		`no problem names ${named.join(", ")}; got:\n${found.join("\n") || "(none)"}`,
	);
	return line;
}

/** Asserts exactly one problem, and that it names each of `named`. */
function only(value: Project, ...named: string[]): string {
	const found = problems(value);
	assert.equal(
		found.length,
		1,
		`expected one problem, got:\n${found.join("\n")}`,
	);
	return names(value, ...named);
}

test("a wire pointing at a node that does not exist is refused by name", () => {
	// The case the whole validator exists for. A silent no-op here presents
	// much later as a node that mysteriously never runs.
	const line = only(
		withWire({
			id: "exec-nowhere",
			from: { node: "recycle", port: "out" },
			to: { node: "nonesuch", port: "in" },
			kind: "exec",
		}),
		"exec-nowhere",
		"nonesuch",
	);
	assert.ok(line.includes("does not hold"), line);
});

test("a wire landing on a port the kind has not got is refused by name", () => {
	names(
		withWire({
			id: "data-wrong-port",
			from: { node: "ring", port: "colour" },
			to: { node: "integrate", port: "position" },
			kind: "data",
		}),
		"data-wrong-port",
		"colour",
		"ring",
	);
});

test("a data wire between mismatched port types is refused", () => {
	const line = only(
		withWire({
			id: "data-mismatched",
			from: { node: "recycle", port: "position" },
			to: { node: "draw", port: "viewProjection" },
			kind: "data",
		}),
		"data-mismatched",
	);
	assert.ok(
		line.includes("no input named") || line.includes("column<vec3>"),
		line,
	);
});

test("two wires feeding one input are refused rather than one winning quietly", () => {
	names(
		withWire({
			id: "data-second-source",
			from: { node: "recycle", port: "position" },
			to: { node: "integrate", port: "position" },
			kind: "data",
		}),
		"data-second-source",
		"integrate.position",
	);
});

test("a node run by two things is refused, because the order would be a guess", () => {
	names(
		withWire({
			id: "exec-second",
			from: { node: "recycle", port: "out" },
			to: { node: "integrate", port: "in" },
			kind: "exec",
		}),
		"exec-second",
		"integrate",
	);
});

test("nothing may run an entry point", () => {
	const line = names(
		withWire({
			id: "exec-into-step",
			from: { node: "recycle", port: "out" },
			to: { node: "step", port: "in" },
			kind: "exec",
		}),
		"exec-into-step",
		"step",
	);
	assert.ok(line.includes("entry point"), line);
});

test("an execution wire across two lanes is refused", () => {
	// A lane is when a node runs, so this would claim a node runs twice.
	names(
		withWire({
			id: "exec-across",
			from: { node: "recycle", port: "out" },
			to: { node: "draw", port: "in" },
			kind: "exec",
		}),
		"exec-across",
		"step",
		"paint",
	);
});

test("execution that runs in a ring is refused", () => {
	const nodes = root().nodes.filter((node) => node.lane === "step");
	const wires: Wire[] = [
		{
			id: "ring-a",
			from: { node: "integrate", port: "out" },
			to: { node: "recycle", port: "in" },
			kind: "exec",
		},
		{
			id: "ring-b",
			from: { node: "recycle", port: "out" },
			to: { node: "integrate", port: "in" },
			kind: "exec",
		},
	];
	const found = problems(project(nodes, wires));
	assert.ok(
		found.some((line) => line.includes("ring")),
		`a cycle should be reported, got:\n${found.join("\n")}`,
	);
});

test("a node of an unknown kind is refused by name", () => {
	const nodes = root().nodes.map((node) =>
		node.id === "recycle" ? { ...node, kind: "teleport" } : node,
	);
	const found = problems(project(nodes, [...root().wires]));
	assert.ok(
		found.some((line) => line.includes("recycle") && line.includes("teleport")),
		`the offending node and kind should both be named, got:\n${found.join("\n")}`,
	);
});

test("an entry point outside its own lane is refused", () => {
	const nodes = root().nodes.map((node) =>
		node.id === "paint" ? { ...node, lane: "step" as const } : node,
	);
	const found = problems(project(nodes, [...root().wires]));
	assert.ok(
		found.some((line) => line.includes("paint")),
		found.join("\n"),
	);
});

test("a param the kind does not declare, or one out of range, is refused", () => {
	const unknown = root().nodes.map((node) =>
		node.id === "ring" ? { ...node, params: { colour: "red" } } : node,
	);
	only(project(unknown, [...root().wires]), "ring", "colour");

	const negative = root().nodes.map((node) =>
		node.id === "ring" ? { ...node, params: { radius: -1 } } : node,
	);
	only(project(negative, [...root().wires]), "ring", "radius");

	const fractional = root().nodes.map((node) =>
		node.id === "ring" ? { ...node, params: { count: 2.5 } } : node,
	);
	only(project(fractional, [...root().wires]), "ring", "count");
});

test("a node opening into a board the project has not got is refused by name", () => {
	const nodes = root().nodes.map((node) =>
		node.id === "draw" ? { ...node, board: "elsewhere" } : node,
	);
	only(project(nodes, [...root().wires]), "draw", "elsewhere");
});

test("a board that opens into itself is refused", () => {
	const nodes = root().nodes.map((node) =>
		node.id === "draw" ? { ...node, board: ROOT_BOARD } : node,
	);
	const found = problems(project(nodes, [...root().wires]));
	assert.ok(found.length > 0, "an endless breadcrumb should be refused");
});

test("a project opening on a board it does not hold is refused", () => {
	const found = problems({ ...TRIANGLES, root: "nonesuch" });
	assert.equal(found.length, 1);
	assert.ok((found[0] as string).includes("nonesuch"), found[0]);
});

test("a document from a newer version is refused rather than half-read", () => {
	const found = problems({ ...TRIANGLES, version: DOCUMENT_VERSION + 1 });
	assert.equal(found.length, 1);
	assert.ok(
		(found[0] as string).includes(String(DOCUMENT_VERSION + 1)),
		"the message should say which version it got",
	);
});

test("parsing something that is not a document fails saying what is wrong", () => {
	const refused = (text: string, ...named: string[]): void => {
		let error: unknown;
		try {
			parseProject(text);
		} catch (thrown) {
			error = thrown;
		}
		assert.ok(
			error instanceof BoardError,
			`${text} was read as a document — callers narrow on this type`,
		);
		assert.ok(error.problems.length > 0, "an error carries its problems");
		for (const name of named) {
			assert.ok(
				error.message.includes(name),
				`the message does not name \`${name}\`: ${error.message}`,
			);
		}
	};

	refused("{", "JSON");
	refused("[]", "object");
	refused(JSON.stringify({ version: 1, root: "a" }), "boards");
	refused(
		JSON.stringify({ version: 1, root: "a", boards: { a: { title: "a" } } }),
		"nodes",
	);
	refused(
		JSON.stringify({
			version: 1,
			root: "a",
			boards: { a: { title: "a", nodes: [{ id: "n" }], wires: [] } },
		}),
		"kind",
		"n",
	);
	refused(
		JSON.stringify({
			version: 1,
			root: "a",
			boards: {
				a: {
					title: "a",
					nodes: [{ id: "n", kind: "start", lane: "elsewhere", column: 0 }],
					wires: [],
				},
			},
		}),
		"lane",
	);
	refused(
		JSON.stringify({
			version: 1,
			root: "a",
			boards: {
				a: { title: "a", nodes: [], wires: [{ id: "w", kind: "sideways" }] },
			},
		}),
		"w",
		"exec",
	);
});

test("a document cannot reach past its own contents through a key", () => {
	// `kind` is a string a document chose, and a plain object indexed by one
	// answers for `constructor` and `toString` as readily as for a real kind.
	// A lookup that walked the prototype chain would resolve `toString` to a
	// function and take it for a node kind.
	const nodes: BoardNode[] = [
		{ id: "n", kind: "toString", lane: "start", column: 0 },
	];
	only(project(nodes, []), "n", "toString");

	// Written as text rather than through `JSON.stringify`, because
	// `{ __proto__: … }` in a literal sets the prototype instead of a key and
	// there would be nothing left to serialise. A document off disk has no
	// such courtesy.
	const text = `{
		"version": ${DOCUMENT_VERSION},
		"root": "a",
		"boards": {
			"a": {
				"title": "a",
				"nodes": [{
					"id": "n", "kind": "start", "lane": "start", "column": 0,
					"__proto__": { "polluted": true }
				}],
				"wires": []
			}
		}
	}`;
	const read = parseProject(text);
	assert.deepEqual(
		read.boards.a?.nodes[0],
		{ id: "n", kind: "start", lane: "start", column: 0 },
		"a key the schema does not name must not survive the read",
	);
	assert.equal(
		({} as Record<string, unknown>).polluted,
		undefined,
		"reading a document must not touch Object.prototype",
	);
});

test("a round trip through text survives an edited document", () => {
	// Not the fixture this time: the fixture is the happy path, and a saved
	// project is whatever the last edit left behind.
	const edited = project(
		root().nodes.map((node) =>
			node.id === "ring" ? { ...node, params: { count: 3, radius: 1 } } : node,
		),
		root().wires.filter((wire) => wire.kind === "exec"),
	);
	assert.deepEqual(validate(edited), []);
	assert.deepEqual(parseProject(serializeProject(edited)), edited);
});
