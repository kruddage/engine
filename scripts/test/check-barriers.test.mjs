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

/* A two-package workspace: `packages/engine` (privileged) and `packages/other`.
 * Both rules read sources, so a package here is a name and a `src/`. */
function workspace(files) {
	const root = mkdtempSync(join(tmpdir(), "krudd-ws-"));

	for (const [dir, name] of [
		["engine", "@kruddage/engine"],
		["other", "@kruddage/other"],
	]) {
		mkdirSync(join(root, "packages", dir, "src"), { recursive: true });
		writeFileSync(
			join(root, "packages", dir, "package.json"),
			JSON.stringify({ name })
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

/* Rule 3 carries rule 2's weight since #934, so the cases below walk every
 * subtree and every variable the patterns name rather than one of each. What
 * used to be caught twice — once as a declared dependency on the build package,
 * once as a path — is now caught only here. */
test("catches a non-engine package reaching krudd/ by path", () => {
	for (const path of [
		"krudd/kruddmake/manifest.scm",
		"krudd/engine/abi/build.scm",
		"krudd/third_party/s7.h",
	]) {
		const problems = check({
			"packages/other/src/a.mjs": `readFileSync("${path}");\n`,
		});

		assert.equal(problems.length, 1, path);
		assert.match(problems[0], /krudd\/ by path/);
	}
});

/* The path rule is what stands behind the deleted dependency rule: a package
 * that wants to drive the build has to spell a path to reach kruddmake now that
 * there is no package name to ask for, and that path is what this catches. */
test("catches a non-engine package deriving the kruddmake entry point", () => {
	const problems = check({
		"packages/other/src/a.mjs":
			`const sh = join(REPO, "krudd/kruddmake/kruddmake.sh");\n` +
			`spawnSync("sh", [sh, "build"]);\n`,
	});

	assert.equal(problems.length, 1);
	assert.match(problems[0], /krudd\/ by path/);
});

/* The rule that used to match this string is gone: it was the repo-root
 * forwarding shim's name, a package boundary drawn with a regex, and the shim
 * itself is retired (#935). Naming a deleted file is not an act. */
test("naming the retired shim is not on its own a violation", () => {
	assert.deepEqual(
		check({
			"packages/other/src/a.mjs": `const shim = "krudd.sh";\nexport { shim };\n`,
		}),
		[]
	);
});

test("catches a non-engine package setting the build environment", () => {
	for (const variable of ["KRUDD_TARGET", "KRUDD_BUILD_DIR"]) {
		const problems = check({
			"packages/other/src/a.mjs": `process.env.${variable} = "x";\n`,
		});

		assert.equal(problems.length, 1, variable);
		assert.match(problems[0], /kruddmake build environment/);
	}
});

/* The exemption, which is now the only thing separating @kruddage/engine from
 * every other package — see the note beside ENGINE_PRIVATE. It has to cover the
 * real shape of packages/engine/scripts/kruddmake.mjs, which derives krudd/ from
 * the repo root and spawns the entry point it finds there. */
test("the engine package may do all of that", () => {
	assert.deepEqual(
		check({
			"packages/engine/src/b.mjs":
				`const sh = join(REPO, "krudd", "kruddmake", "kruddmake.sh");\n` +
				`spawnSync("sh", [sh, "build"], {\n` +
				`	env: { KRUDD_TARGET: "wasm", KRUDD_BUILD_DIR: out },\n` +
				`});\n` +
				`readFileSync("krudd/kruddmake/manifest.scm");\n`,
		}),
		[]
	);
});

test("comments naming a forbidden path are not violations", () => {
	assert.deepEqual(
		check({
			"packages/other/src/a.mjs":
				`// This never reads krudd/kruddmake/.\n` +
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

/* The C tree left the workspace with #934, and this is what says so out loud.
 * The list is asserted whole rather than by absence: `!includes("krudd/...")`
 * would pass just as well if packageDirs returned nothing at all, and a
 * boundary check over an empty list is the failure mode worth catching. */
test("the C tree is not in the workspace", () => {
	const dirs = packageDirs(REPO);

	assert.deepEqual(dirs.sort(), [
		"packages/engine",
		"packages/site",
		"tools/render-diff",
	]);
});

/* `packages/*` is expanded off the filesystem, mirroring the glob of the same
 * shape in pnpm-workspace.yaml: a new package under packages/ is inside the
 * check the moment it exists, rather than when someone remembers this list. A
 * package the boundary check does not know about is a package with no
 * boundary. */
test("packages/ is discovered, not enumerated", () => {
	const root = mkdtempSync(join(tmpdir(), "krudd-ws-"));
	for (const name of ["alpha", "beta"]) {
		mkdirSync(join(root, "packages", name), { recursive: true });
	}

	assert.deepEqual(packageDirs(root).sort(), [
		"packages/alpha",
		"packages/beta",
		"tools/render-diff",
	]);
});
