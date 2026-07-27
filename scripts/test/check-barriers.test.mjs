// SPDX-License-Identifier: GPL-2.0-or-later
//
// The check is exercised against synthetic workspaces, plus one assertion
// against the real one — a boundary check that has never been shown to fail is
// indistinguishable from a boundary check that cannot fail.

import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { test } from "node:test";

import { findViolations, packageDirs } from "../check-barriers.mjs";

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");

/* A two-package workspace: `packages/engine` (privileged) and `packages/other`. */
function workspace(files) {
	const root = mkdtempSync(join(tmpdir(), "krudd-ws-"));

	for (const [name, manifest] of [
		["engine", { name: "@kruddage/engine" }],
		["other", { name: "@kruddage/other" }],
	]) {
		mkdirSync(join(root, "packages", name, "src"), { recursive: true });
		writeFileSync(
			join(root, "packages", name, "package.json"),
			JSON.stringify(manifest)
		);
	}

	for (const [path, source] of Object.entries(files)) {
		mkdirSync(dirname(join(root, path)), { recursive: true });
		writeFileSync(join(root, path), source);
	}

	return {
		root,
		dirs: ["packages/engine", "packages/other"],
	};
}

function check(files) {
	const { root, dirs } = workspace(files);
	return findViolations(root, dirs);
}

test("a clean workspace reports nothing", () => {
	assert.deepEqual(
		check({
			"packages/other/src/a.mjs": `import { artifacts } from "@kruddage/engine";\nartifacts();\n`,
			"packages/engine/src/b.mjs": `export const x = 1;\n`,
		}),
		[]
	);
});

test("catches a relative import that escapes the package", () => {
	const problems = check({
		"packages/other/src/a.mjs": `import { x } from "../../engine/src/b.mjs";\n`,
	});

	assert.equal(problems.length, 1);
	assert.match(problems[0], /escapes @kruddage\/other/);
});

test("allows relative imports inside the package", () => {
	assert.deepEqual(
		check({
			"packages/other/src/a.mjs": `import { x } from "./b.mjs";\nimport { y } from "../src/c.mjs";\n`,
		}),
		[]
	);
});

test("catches a dynamic import that escapes the package", () => {
	const problems = check({
		"packages/other/src/a.mjs": `const m = await import("../../engine/src/b.mjs");\n`,
	});

	assert.equal(problems.length, 1);
	assert.match(problems[0], /escapes/);
});

test("catches a non-engine package driving the engine build", () => {
	const problems = check({
		"packages/other/src/a.mjs": `spawnSync("./krudd.sh", ["build"]);\n`,
	});

	assert.equal(problems.length, 1);
	assert.match(problems[0], /krudd\.sh.*private to @kruddage\/engine/s);
});

test("catches a non-engine package reading krudd/ internals", () => {
	const problems = check({
		"packages/other/src/a.mjs": `readFileSync("krudd/kruddmake/manifest.scm");\n`,
	});

	assert.equal(problems.length, 1);
	assert.match(problems[0], /krudd\/ internals/);
});

test("catches a non-engine package setting the build environment", () => {
	const problems = check({
		"packages/other/src/a.mjs": `process.env.KRUDD_TARGET = "wasm";\n`,
	});

	assert.equal(problems.length, 1);
	assert.match(problems[0], /kruddmake build environment/);
});

test("the engine package may do all of that", () => {
	assert.deepEqual(
		check({
			"packages/engine/src/b.mjs":
				`spawnSync("./krudd.sh", ["build"], { env: { KRUDD_TARGET: "wasm" } });\n` +
				`readFileSync("krudd/kruddmake/manifest.scm");\n`,
		}),
		[]
	);
});

test("comments naming a forbidden path are not violations", () => {
	assert.deepEqual(
		check({
			"packages/other/src/a.mjs":
				`// This never runs krudd.sh and never reads krudd/kruddmake/.\n` +
				`/* KRUDD_TARGET is the engine's business. */\n` +
				`export const x = 1;\n`,
		}),
		[]
	);
});

test("a package with no readable manifest is reported", () => {
	const root = mkdtempSync(join(tmpdir(), "krudd-ws-"));
	mkdirSync(join(root, "packages", "ghost"), { recursive: true });

	const problems = findViolations(root, ["packages/ghost"]);
	assert.equal(problems.length, 1);
	assert.match(problems[0], /no readable package\.json/);
});

test("this workspace has no boundary violations", () => {
	assert.deepEqual(findViolations(REPO), []);
});

/* The check discovers C packages the same way pnpm does — by finding a manifest
 * in the tree — so a module that joins the workspace cannot skip the boundary
 * check by not being added to a second list. */
test("packages in the C tree are discovered, not listed", () => {
	const dirs = packageDirs(REPO);

	assert.ok(dirs.includes("krudd/engine/abi"));
	assert.ok(dirs.includes("packages/engine"));
	assert.ok(!dirs.includes("krudd/engine/base"));
});
