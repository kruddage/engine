// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Which project a URL opens.
 *
 * Small enough to look untestable and load-bearing enough not to be: the page
 * boots one of these and `render-test` compares its screenshot against a
 * reference of the other, so "`?project=` and no parameter at all mean the
 * same thing" and "an unknown name is refused" are both facts something
 * depends on.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import { TICTACTOE, TRIANGLES, validate } from "@krudd/board";
import { chosenProject, DEFAULT_PROJECT, PROJECTS } from "../src/projects";

test("a page with nothing in its query string opens the default", () => {
	assert.equal(chosenProject(""), PROJECTS[DEFAULT_PROJECT]);
	assert.equal(chosenProject("?"), PROJECTS[DEFAULT_PROJECT]);
	// An empty value is somebody who cleared the name, not somebody who asked
	// for a project called "".
	assert.equal(chosenProject("?project="), PROJECTS[DEFAULT_PROJECT]);
});

test("the page opens on tic-tac-toe and keeps the demo a link away", () => {
	assert.equal(chosenProject(""), TICTACTOE);
	assert.equal(chosenProject("?project=triangles"), TRIANGLES);
	assert.equal(
		chosenProject("?other=1&project=triangles"),
		TRIANGLES,
		"the parameter is found wherever in the query string it sits",
	);
});

test("a name the page does not hold is refused, and says what it holds", () => {
	assert.throws(
		() => chosenProject("?project=tictactoe"),
		/no project named `tictactoe`.*tic-tac-toe, triangles/s,
		"a near miss must not quietly open something else",
	);
	// A document is untrusted input and a query string is more so: indexing a
	// plain object with a prototype key must not hand back something that is
	// not a project — the same `hasOwn` rule `kindOf` follows.
	assert.throws(() => chosenProject("?project=constructor"));
});

test("every project the page can open is one the engine can run", () => {
	for (const [name, project] of Object.entries(PROJECTS)) {
		assert.deepEqual(validate(project), [], `${name} does not hold together`);
	}
});
