// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * A project as a file: what it is called, what survives the round trip, and
 * what a file that cannot be trusted does instead of loading.
 *
 * The round trip is the one that matters, and it is asserted twice over
 * because saving and loading has two halves that can each be right on its own:
 * the **board** comes back — the same bytes, so a project diffs and a save
 * with no edit in it is not a change — and the **game** comes back, which is
 * the reloaded document running to the same numbers, frame for frame. A save
 * that preserved every field and dropped a param default would pass the first
 * and fail the second.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import {
	BoardError,
	cloneProject,
	DEFAULT_PROJECT_NAME,
	DOCUMENT_VERSION,
	PAINT_TO_DRAW,
	PROJECT_EXTENSION,
	PROJECT_MEDIA_TYPE,
	parseProject,
	projectFileName,
	ROOT_BOARD,
	Runner,
	serializeProject,
	setParam,
	TRIANGLES,
	toggleWire,
} from "../src/index";
import { at, TestWorld } from "./world";

/** The frame the render test steps at, and the count it steps. */
const DT = 1 / 60;
const FRAMES = 90;

/** What a project file holds: the document, and nothing around it. */
test("a project file is the document's own bytes", () => {
	const text = serializeProject(TRIANGLES);
	assert.equal(
		JSON.parse(text).version,
		DOCUMENT_VERSION,
		"a reader with no krudd in it can still see what version this is",
	);
	assert.equal(PROJECT_MEDIA_TYPE, "application/json", "because that is what");
	assert.equal(PROJECT_EXTENSION, ".krudd");
});

test("a project file is named for what it holds", () => {
	assert.equal(projectFileName("triangles"), "triangles.krudd");
	assert.equal(
		projectFileName("triangles.krudd"),
		"triangles.krudd",
		"a name that already carries the extension does not take a second one",
	);
	assert.equal(projectFileName("triangles.krudd.krudd"), "triangles.krudd");
	assert.equal(
		projectFileName("worlds/the best one"),
		"worlds the best one.krudd",
		"a slash in a title is a path, and a path saves somewhere nobody asked",
	);
	assert.equal(
		projectFileName(".krudd"),
		`${DEFAULT_PROJECT_NAME}${PROJECT_EXTENSION}`,
		"and a name that is only the extension is a hidden file, not a save",
	);
	assert.equal(
		projectFileName(),
		`${DEFAULT_PROJECT_NAME}${PROJECT_EXTENSION}`,
	);
	assert.equal(projectFileName("   "), `${DEFAULT_PROJECT_NAME}.krudd`);
});

test("the board comes back out of the file it went into", () => {
	const text = serializeProject(TRIANGLES);
	const reopened = parseProject(text);

	assert.deepEqual(reopened, TRIANGLES, "every field, not merely a valid one");
	assert.equal(
		serializeProject(reopened),
		text,
		"and the same bytes, so a save with no edit in it is not a change",
	);
});

test("an edited board comes back edited", () => {
	// The edits PR-7 makes, which are the ones a save has to carry: a cut wire
	// and a param that is no longer its kind's default.
	const edited = cloneProject(TRIANGLES);
	toggleWire(edited, edited.root, PAINT_TO_DRAW);
	setParam(edited, edited.root, "ring", "count", 3);

	const reopened = parseProject(serializeProject(edited));
	assert.deepEqual(reopened, edited);

	const root = reopened.boards[reopened.root];
	assert.equal(
		root?.wires.find((wire) => wire.id === PAINT_TO_DRAW)?.cut,
		true,
		"a cut wire that came back whole would be an edit the file dropped",
	);
	assert.equal(
		root?.nodes.find((node) => node.id === "ring")?.params?.count,
		3,
	);
});

test("the game comes back too, frame for frame", () => {
	// The half a field-by-field comparison cannot see: the reopened document
	// has to *run* the same. Same frames, same dt, same numbers out.
	const first = new TestWorld();
	const before = new Runner(TRIANGLES, first);
	before.start();

	const second = new TestWorld();
	const after = new Runner(parseProject(serializeProject(TRIANGLES)), second);
	after.start();

	for (let i = 0; i < FRAMES; i++) {
		before.frame(DT);
		after.frame(DT);
	}

	assert.equal(second.slotCount, first.slotCount, "the same world was spawned");
	for (let slot = 0; slot < first.slotCount; slot++) {
		assert.deepEqual(
			at(second, slot),
			at(first, slot),
			`entity ${slot} is somewhere else after the round trip`,
		);
	}
	assert.equal(second.draws, first.draws, "and drawn the same number of times");
	assert.equal(second.blanks, first.blanks);
	assert.equal(second.scale, first.scale);
});

test("a cut wire is still cut when the project is opened again", () => {
	const edited = cloneProject(TRIANGLES);
	toggleWire(edited, edited.root, PAINT_TO_DRAW);

	const world = new TestWorld();
	const runner = new Runner(parseProject(serializeProject(edited)), world);
	runner.start();
	runner.frame(DT);

	assert.equal(world.draws, 0, "a save that reconnected the wire is a save");
	assert.equal(world.blanks, 1, "that would have put the triangles back");
});

test("a file from a newer krudd is refused, by version", () => {
	const ahead = serializeProject({
		...TRIANGLES,
		version: DOCUMENT_VERSION + 1,
	});

	const error = refused(ahead);
	assert.match(
		error.message,
		new RegExp(`version ${DOCUMENT_VERSION + 1}`),
		"it says which version it got",
	);
	assert.match(
		error.message,
		new RegExp(`version ${DOCUMENT_VERSION}`),
		"and which one it reads, which is what makes it actionable",
	);
});

test("a file that is not a document is refused, saying what it is not", () => {
	assert.match(refused("{").message, /not valid JSON/);
	assert.match(refused("").message, /not valid JSON/);
	assert.match(refused("[]").message, /object/, "a list of nothing is not one");
	assert.match(refused('{"version": 1}').message, /root/);
});

test("a file whose board does not hold together names the wire", () => {
	const root = TRIANGLES.boards[ROOT_BOARD];
	assert.ok(root !== undefined);
	const dangling = serializeProject({
		...TRIANGLES,
		boards: {
			...TRIANGLES.boards,
			[ROOT_BOARD]: {
				...root,
				wires: root.wires.map((wire) =>
					wire.id === PAINT_TO_DRAW
						? { ...wire, to: { node: "nowhere", port: "in" } }
						: wire,
				),
			},
		},
	});

	const error = refused(dangling);
	assert.match(error.message, new RegExp(PAINT_TO_DRAW), "the wire, by id");
	assert.match(error.message, /nowhere/, "and the node it points at");
});

test("every problem in a bad file, not the first one", () => {
	const root = TRIANGLES.boards[ROOT_BOARD];
	assert.ok(root !== undefined);
	const broken = serializeProject({
		...TRIANGLES,
		boards: {
			...TRIANGLES.boards,
			[ROOT_BOARD]: {
				...root,
				wires: root.wires.map((wire) => ({
					...wire,
					to: { node: "nowhere", port: wire.to.port },
				})),
			},
		},
	});

	assert.ok(
		refused(broken).problems.length > 1,
		"a document with four things wrong is four things to fix, not one",
	);
});

/** The `BoardError` reading `text` throws, or a failure saying it did not. */
function refused(text: string): BoardError {
	try {
		parseProject(text);
	} catch (error) {
		assert.ok(error instanceof BoardError, `refused with ${String(error)}`);
		return error;
	}
	assert.fail("the document was read rather than refused");
}
