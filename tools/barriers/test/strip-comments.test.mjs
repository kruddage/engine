// SPDX-License-Identifier: GPL-2.0-or-later

import assert from "node:assert/strict";
import { test } from "node:test";

import { stripComments } from "../strip-comments.mjs";

/* This file lives inside @kruddage/barriers (#939), so it is itself walked by
 * check-barriers.mjs's rule 3 on every `workspace.sh check`. One fixture below wants a
 * "krudd/kruddmake/" inside a fake block comment; assembled here instead of
 * spelled out in place, so the text this file's own bytes offer to that walk
 * never contains it contiguous — see the longer version of this note in
 * check-barriers.test.mjs. */
const KRUDDMAKE_PATH = "krudd" + "/kruddmake/";

test("removes line comments, keeps the code around them", () => {
	const out = stripComments('const a = 1; // mentions krudd.sh\nconst b = 2;');
	assert.match(out, /const a = 1;/);
	assert.match(out, /const b = 2;/);
	assert.doesNotMatch(out, /krudd\.sh/);
});

test("removes block comments spanning lines", () => {
	const out = stripComments(`/* krudd.sh\n * ${KRUDDMAKE_PATH}\n */\nlet x;`);
	assert.doesNotMatch(out, /krudd/);
	assert.match(out, /let x;/);
});

test("keeps string literals, including ones that look like comments", () => {
	const out = stripComments(`const url = "https://example.com/a//b";`);
	assert.match(out, /https:\/\/example\.com\/a\/\/b/);
});

test("keeps a forbidden name that really is in the code", () => {
	const out = stripComments(`spawnSync("./krudd.sh", ["build"]);`);
	assert.match(out, /krudd\.sh/);
});

test("does not mistake a regex literal for a comment", () => {
	/* The `/*` inside this pattern would open a block comment to a naive
	 * stripper and swallow the rest of the file. */
	const out = stripComments("const re = /a[/*]b/; const after = 1;");
	assert.match(out, /const after = 1;/);
});

test("handles a regex containing an escaped slash", () => {
	const out = stripComments("const re = /\\/\\.wasm$/; const after = 2;");
	assert.match(out, /const after = 2;/);
});

test("handles division without treating it as a regex", () => {
	const out = stripComments("const half = total / 2; // note\nconst n = 3;");
	assert.match(out, /total \/ 2/);
	assert.match(out, /const n = 3;/);
	assert.doesNotMatch(out, /note/);
});

test("keeps template literals", () => {
	const out = stripComments("const s = `path: ${dir}/krudd.sh`;");
	assert.match(out, /krudd\.sh/);
});

test("preserves line numbering", () => {
	const source = "a;\n/* one\n   two */\nb;";
	assert.equal(stripComments(source).split("\n").length, source.split("\n").length);
});

test("an escaped quote does not end a string early", () => {
	const out = stripComments('const s = "he said \\"// not a comment\\""; const t = 1;');
	assert.match(out, /not a comment/);
	assert.match(out, /const t = 1;/);
});
