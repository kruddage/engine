// SPDX-License-Identifier: GPL-2.0-or-later

import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { test } from "node:test";

import { ENGINE_ARTIFACTS, bustedName } from "../src/artifacts.mjs";
import { bakedCacheStem } from "../src/index.mjs";

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");

test("cache-busting inserts the stem before the extension", () => {
	const wasm = ENGINE_ARTIFACTS.find((a) => a.name === "index.wasm");
	assert.equal(bustedName(wasm, "abc1234"), "index.abc1234.wasm");
});

test("artifacts that are not cache-busted keep their names", () => {
	for (const artifact of ENGINE_ARTIFACTS.filter((a) => !a.cacheBusting)) {
		assert.equal(bustedName(artifact, "abc1234"), artifact.name);
	}
});

test("every artifact declares a role the site knows how to stage", () => {
	for (const artifact of ENGINE_ARTIFACTS) {
		assert.ok(
			["entry", "code", "static"].includes(artifact.role),
			`${artifact.name} has role ${artifact.role}`
		);
	}
});

test("exactly one artifact is the entry document", () => {
	const entries = ENGINE_ARTIFACTS.filter((a) => a.role === "entry");
	assert.deepEqual(
		entries.map((e) => e.name),
		["index.html"]
	);
});

test("only files the entry document names are rewritten into it", () => {
	/* index.wasm is renamed but must not be rewritten: the shell resolves it
	 * through Module.locateFile at runtime, and the sole literal mention in the
	 * HTML is the error overlay's display text. */
	const rewritten = ENGINE_ARTIFACTS.filter((a) => a.rewriteInEntry).map(
		(a) => a.name
	);
	assert.deepEqual(rewritten, ["index.js"]);
});

test("a rewritten artifact is always also cache-busted", () => {
	/* Rewriting a name that never changes would be a no-op the staging step
	 * would then fail on, having found nothing to replace. */
	for (const artifact of ENGINE_ARTIFACTS.filter((a) => a.rewriteInEntry)) {
		assert.ok(artifact.cacheBusting, `${artifact.name} is rewritten but not busted`);
	}
});

test("the declared cache-busting set matches the shell's locateFile hook", () => {
	/* The engine says index.wasm is renamed at deploy time. That is only true
	 * because the shell template rewrites .wasm requests through a stem baked in
	 * at configure time. If that hook is ever removed from the template, this
	 * table is lying and every deploy 404s on the module. */
	const template = readFileSync(
		join(REPO, "krudd/engine/shell/web/shell.html.in"),
		"utf8"
	);

	assert.equal(
		bakedCacheStem(template),
		"@GIT_COMMIT_HASH@",
		"shell.html.in must resolve .wasm through a configure-time stem"
	);

	const wasm = ENGINE_ARTIFACTS.find((a) => a.name === "index.wasm");
	assert.ok(wasm.cacheBusting);
});

test("bakedCacheStem returns null when the hook is absent", () => {
	assert.equal(bakedCacheStem("<html><body>nothing here</body></html>"), null);
});
