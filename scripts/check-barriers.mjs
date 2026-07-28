// SPDX-License-Identifier: GPL-2.0-or-later
//
// Workspace boundary check.
//
// The reason for splitting this repo into packages is to get barriers that hold
// on their own, rather than by everyone remembering where the lines are. This
// is what makes them hold.
//
// Three rules:
//
//   1. A package may not reach into another package by relative path. Cross-
//      package code travels through the package name, which means it travels
//      through the "exports" map, which means the importing side can only see
//      what the imported side chose to publish. A `../../engine/src/internal`
//      import routes around all of that, and it is the way every module system
//      erodes.
//
//   2. Only @kruddage/engine may depend on @kruddage/kruddmake. The build
//      language is a package now (#920), so "only @kruddage/engine may drive
//      the engine build" is a statement about the dependency graph, and this
//      is what reads it back. A package that has not declared the dependency
//      cannot resolve it.
//
//   3. Nothing but @kruddage/engine may reach the build tree out of band —
//      by a path into krudd/, or through the generator's environment
//      (KRUDD_TARGET, KRUDD_BUILD_DIR). Rule 2 has an opinion about naming
//      kruddmake, and rule 1 about importing across a package boundary;
//      neither can see a spawn of a path or an env var read, which is what is
//      left for a text match to do.
//
// Rule 3 used to carry a `krudd.sh` pattern as well, and no longer does. That
// pattern was a package boundary drawn with a regex because the thing it
// protected had no package to be inside of; krudd.sh is now a forwarding shim
// over the entry point of one, so matching its name would be a second
// mechanism for what rule 2 enforces. Two mechanisms for one rule is how the
// second one rots.
//
// Run: pnpm check

import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { stripComments } from "./strip-comments.mjs";

const SKIP_DIRS = new Set(["node_modules", "dist", "out", "shots", ".git"]);
const SOURCE_EXT = /\.(mjs|js|cjs|ts|mts)$/;

/* A relative specifier in an import/export/require. */
const RELATIVE_IMPORT =
	/(?:^|[^\w$])(?:import|export)[\s\S]{0,200}?from\s*["'](\.[^"']*)["']|(?:^|[^\w$])(?:import|require)\s*\(\s*["'](\.[^"']*)["']\s*\)/g;

/* The build package. Only @kruddage/engine may name it (rule 2). */
const KRUDDMAKE = "@kruddage/kruddmake";

const DEPENDENCY_FIELDS = [
	"dependencies",
	"devDependencies",
	"peerDependencies",
	"optionalDependencies",
];

/* The engine build reached out of band, past both graph rules (rule 3). */
const ENGINE_PRIVATE = [
	{
		pattern: /\bkrudd\/(kruddmake|engine|third_party)\//,
		what: "krudd/ by path",
	},
	{ pattern: /\bKRUDD_(TARGET|BUILD_DIR)\b/, what: "the kruddmake build environment" },
];

/**
 * Workspace package directories, relative to the repo root. Mirrors
 * pnpm-workspace.yaml — including its `krudd/engine/**` glob, which is why that
 * tree is walked for manifests rather than listed. A C module joins the
 * workspace by gaining a package.json beside its build.scm (#918, Q1), and it
 * has to join this check at the same moment: a package the boundary check does
 * not know about is a package with no boundary.
 *
 * The walk starts at `krudd`, not `krudd/engine`, because @kruddage/kruddmake
 * lives beside the engine tree rather than in it (#920) — and it is the package
 * rule 2 is about, so it is the last one that may be invisible here.
 *
 * These packages hold no JS: kruddmake is Scheme behind a POSIX shell entry
 * point, and the C modules are declarations. Rules 1 and 3 read source files
 * and so pass over them in silence; rule 2 reads manifests and does not.
 */
export function packageDirs(repo) {
	return [
		...readdirSync(join(repo, "packages")).map((d) => join("packages", d)),
		"tools/render-diff",
		...manifestDirs(repo, "krudd"),
	];
}

/** Directories at or below `root` that hold a package.json, repo-relative. */
function manifestDirs(repo, root) {
	const out = [];
	const walk = (relative) => {
		const abs = join(repo, relative);
		for (const name of readdirSync(abs)) {
			if (SKIP_DIRS.has(name)) continue;
			const child = join(relative, name);
			if (!statSync(join(repo, child)).isDirectory()) continue;
			if (existsSync(join(repo, child, "package.json"))) out.push(child);
			walk(child);
		}
	};
	walk(root);
	return out;
}

function sourceFiles(dir) {
	const out = [];
	const walk = (current) => {
		for (const name of readdirSync(current)) {
			if (SKIP_DIRS.has(name)) continue;
			const path = join(current, name);
			if (statSync(path).isDirectory()) walk(path);
			else if (SOURCE_EXT.test(name)) out.push(path);
		}
	};
	walk(dir);
	return out;
}

/**
 * Boundary violations across the workspace, as human-readable strings.
 *
 * @param {string} repo repo root
 * @param {string[]} dirs package directories relative to it
 */
export function findViolations(repo, dirs = packageDirs(repo)) {
	const problems = [];

	for (const packageDir of dirs) {
		const abs = join(repo, packageDir);
		let manifest;
		try {
			manifest = JSON.parse(readFileSync(join(abs, "package.json"), "utf8"));
		} catch {
			problems.push(`${packageDir}: no readable package.json`);
			continue;
		}

		const isEngine = manifest.name === "@kruddage/engine";

		/* Rule 2. Every dependency field, not just "dependencies": moving the
		 * spec to devDependencies would resolve exactly the same and route
		 * around a check that only looked at one of them. */
		if (!isEngine) {
			for (const field of DEPENDENCY_FIELDS) {
				if (!manifest[field]?.[KRUDDMAKE]) continue;
				problems.push(
					`${packageDir}: declares ${KRUDDMAKE} in "${field}". Only ` +
						`@kruddage/engine may drive the engine build; ask it for ` +
						`build outputs instead of producing them here.`
				);
			}
		}

		for (const file of sourceFiles(abs)) {
			const where = relative(repo, file);
			/* Comments are stripped first: the names these rules forbid are the
			 * same names the prose around them has to name in order to explain
			 * the rule. */
			const text = stripComments(readFileSync(file, "utf8"));

			for (const match of text.matchAll(RELATIVE_IMPORT)) {
				const specifier = match[1] ?? match[2];
				const target = resolve(dirname(file), specifier);
				if (target === abs || target.startsWith(abs + "/")) continue;

				problems.push(
					`${where}: imports "${specifier}", which escapes ${manifest.name}. ` +
						`Cross-package code must go through the package name so the ` +
						`"exports" map applies.`
				);
			}

			if (isEngine) continue;

			for (const { pattern, what } of ENGINE_PRIVATE) {
				if (!pattern.test(text)) continue;
				problems.push(
					`${where}: references ${what}, which is private to @kruddage/engine. ` +
						`Ask @kruddage/engine for build outputs instead of producing them here.`
				);
			}
		}
	}

	return problems;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
	const repo = resolve(dirname(fileURLToPath(import.meta.url)), "..");
	const dirs = packageDirs(repo);
	const problems = findViolations(repo, dirs);

	if (problems.length > 0) {
		process.stderr.write("\nworkspace boundary violations:\n\n");
		for (const problem of problems) process.stderr.write(`  ${problem}\n\n`);
		process.exit(1);
	}

	process.stdout.write(
		`workspace boundaries OK (${dirs.length} packages checked)\n`
	);
}
