// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * What the page says when a save or a load did not work.
 *
 * The messages are the testable half of the file layer: the other half is an
 * anchor with a `download` attribute and an `<input type="file">`, which is
 * the browser's own and has nothing in it to assert. What can be got wrong
 * here is what the user is told — a load that failed silently, or one whose
 * message leaves them unsure whether the board in front of them is still the
 * board they had.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import {
	BoardError,
	PROJECT_EXTENSION,
	PROJECT_MEDIA_TYPE,
	serializeProject,
	TRIANGLES,
} from "@krudd/board";
import {
	loadFailure,
	PROJECT_ACCEPT,
	readProjectFile,
	saveFailure,
} from "../src/project-file";

test("the picker offers what a project file is, from one place", () => {
	assert.ok(PROJECT_ACCEPT.includes(PROJECT_EXTENSION));
	assert.ok(PROJECT_ACCEPT.includes(PROJECT_MEDIA_TYPE));
});

test("what opens a file is its bytes, not its name", async () => {
	// The extension moved from `.krudd` to `.json` and nothing had to migrate,
	// because this was always true. Asserted rather than assumed: a name check
	// added later would refuse every project saved before the move, and would
	// do it by telling the user their perfectly good file is the wrong kind.
	const bytes = serializeProject(TRIANGLES);
	for (const name of ["triangles.json", "triangles.krudd", "triangles"]) {
		const opened = await readProjectFile(new File([bytes], name));
		assert.deepEqual(opened, TRIANGLES, `${name} opens the same project`);
	}
});

test("a load failure names the file, the reason, and what is still running", () => {
	const error = new BoardError([
		{ board: "triangles", wire: "exec-paint-draw", message: "it goes nowhere" },
	]);

	const said = loadFailure("borrowed.json", error);
	assert.match(said, /borrowed\.json/, "which file");
	assert.match(said, /exec-paint-draw/, "which wire");
	assert.match(said, /it goes nowhere/, "and what is wrong with it");
	assert.match(
		said,
		/unchanged/,
		"and that the board on screen is the one that was already running",
	);
});

test("a load failure says every problem, not the first", () => {
	const said = loadFailure(
		"broken.json",
		new BoardError([
			{ node: "ring", message: "count must be a whole number" },
			{ node: "integrate", message: "nothing runs it" },
		]),
	);

	assert.match(said, /ring/);
	assert.match(said, /integrate/);
});

test("a failure that is not about the document still says what happened", () => {
	assert.match(
		loadFailure(
			"photo.png",
			new Error("it is not valid JSON: unexpected \x89"),
		),
		/not valid JSON/,
	);
	assert.match(loadFailure("odd.json", "something threw a string"), /string/);
});

test("a save that did not happen says so", () => {
	// A download the browser declined leaves no trace anywhere the user is
	// looking, so silence here reads exactly like success until the file is
	// looked for and is not there.
	const said = saveFailure(new Error("the download was blocked"));
	assert.match(said, /did not save/);
	assert.match(said, /blocked/);
});
