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

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");

/* This file lives inside @kruddage/barriers now, which means it is itself a
 * package the walk below (packageDirs -> findViolations) reads on every
 * `workspace.sh check` — the whole point of #939. Several fixtures below exist to
 * prove rule 3 catches krudd/kruddmake/, krudd/engine/, krudd/third_party/,
 * KRUDD_TARGET and KRUDD_BUILD_DIR; if those strings sat in this file the
 * ordinary way, the check would catch this file for the same reason it is
 * supposed to catch the fixtures it writes into a synthetic workspace two
 * directories down. Assembling them here, once, keeps every fixture below
 * byte-identical to what a real violation looks like without leaving the
 * trigger sitting in this file's own text for the walk to find when it
 * reaches tools/barriers. */
const KRUDDMAKE_PATH = "krudd" + "/kruddmake/";
const ENGINE_PATH = "krudd" + "/engine/";
const THIRD_PARTY_PATH = "krudd" + "/third_party/";
const TARGET_ENV = "KRUDD" + "_TARGET";
const BUILD_DIR_ENV = "KRUDD" + "_BUILD_DIR";
const ESCAPING_SPECIFIER = "../../engine/src/b.mjs";

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
		"packages/other/src/a.mjs": `import { x } from "${ESCAPING_SPECIFIER}";\n`,
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
		"packages/other/src/a.mjs": `const m = await import("${ESCAPING_SPECIFIER}");\n`,
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
		`${KRUDDMAKE_PATH}manifest.scm`,
		`${ENGINE_PATH}abi/build.scm`,
		`${THIRD_PARTY_PATH}s7.h`,
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
			`const sh = join(REPO, "${KRUDDMAKE_PATH}kruddmake.sh");\n` +
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
	for (const variable of [TARGET_ENV, BUILD_DIR_ENV]) {
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
				`	env: { ${TARGET_ENV}: "wasm", ${BUILD_DIR_ENV}: out },\n` +
				`});\n` +
				`readFileSync("${KRUDDMAKE_PATH}manifest.scm");\n`,
		}),
		[]
	);
});

test("comments naming a forbidden path are not violations", () => {
	assert.deepEqual(
		check({
			"packages/other/src/a.mjs":
				`// This never reads ${KRUDDMAKE_PATH}.\n` +
				`/* ${TARGET_ENV} is the engine's business. */\n` +
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
		"tools/barriers",
		"tools/dawn-smoke",
		"tools/render-diff",
	]);
});

/* `packages/*` is expanded off the filesystem rather than listed: a new
 * package under packages/ is inside the check the moment it exists, rather
 * than when someone remembers this list. A package the boundary check does not
 * know about is a package with no boundary. This used to mirror a glob of the
 * same shape in pnpm-workspace.yaml; since #1010 removed that file, discovery
 * is not a mirror of anything — it is where membership is defined. */
test("packages/ is discovered, not enumerated", () => {
	const root = mkdtempSync(join(tmpdir(), "krudd-ws-"));
	for (const name of ["alpha", "beta"]) {
		mkdirSync(join(root, "packages", name), { recursive: true });
	}

	assert.deepEqual(packageDirs(root).sort(), ["packages/alpha", "packages/beta"]);
});

/* tools/* is discovered the same way packages/* is, and by the same
 * manifestDirs walk krudd/ used to be excluded through: a directory under
 * tools/ only joins the boundary check once it declares itself a package.
 * tools/dawn-smoke is the standing example — C and a shell script, no
 * package.json — and it must be silently skipped rather than reported as a
 * package with no manifest. */
test("tools/ is discovered, and a manifest-less tool is skipped", () => {
	const root = mkdtempSync(join(tmpdir(), "krudd-ws-"));
	mkdirSync(join(root, "packages"), { recursive: true });
	mkdirSync(join(root, "tools", "has-manifest"), { recursive: true });
	writeFileSync(
		join(root, "tools", "has-manifest", "package.json"),
		JSON.stringify({ name: "@kruddage/has-manifest" })
	);
	mkdirSync(join(root, "tools", "no-manifest"), { recursive: true });

	assert.deepEqual(packageDirs(root).sort(), ["tools/has-manifest"]);
});

/* A workspace with no tools/ directory at all is not an error: discovery over
 * a directory that is not there contributes nothing, the same way the glob it
 * replaced would have. */
test("a missing tools/ directory is not an error", () => {
	const root = mkdtempSync(join(tmpdir(), "krudd-ws-"));
	mkdirSync(join(root, "packages", "alpha"), { recursive: true });

	assert.deepEqual(packageDirs(root), ["packages/alpha"]);
});
