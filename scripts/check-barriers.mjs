// SPDX-License-Identifier: GPL-2.0-or-later
//
// Workspace boundary check.
//
// The reason for splitting this repo into packages is to get barriers that hold
// on their own, rather than by everyone remembering where the lines are. This
// is what makes them hold.
//
// Two rules:
//
//   1. A package may not reach into another package by relative path. Cross-
//      package code travels through the package name, which means it travels
//      through the "exports" map, which means the importing side can only see
//      what the imported side chose to publish. A `../../engine/src/internal`
//      import routes around all of that, and it is the way every module system
//      erodes.
//
//   2. Only @kruddage/engine may drive the engine build. krudd.sh, krudd/, and
//      the kruddmake output directory are that package's private business.
//      Anything else that wants build outputs asks @kruddage/engine for them.
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

/* Engine-build entry points only @kruddage/engine may reach for. */
const ENGINE_PRIVATE = [
	{ pattern: /\bkrudd\.sh\b/, what: "krudd.sh" },
	{
		pattern: /\bkrudd\/(kruddmake|engine|third_party)\//,
		what: "krudd/ internals",
	},
	{ pattern: /\bKRUDD_(TARGET|BUILD_DIR)\b/, what: "the kruddmake build environment" },
];

/**
 * Workspace package directories, relative to the repo root. Mirrors
 * pnpm-workspace.yaml — including its `krudd/engine/**` glob, which is why the
 * C tree is walked for manifests rather than listed. A C module joins the
 * workspace by gaining a package.json beside its build.scm (#918, Q1), and it
 * has to join this check at the same moment: a package the boundary check does
 * not know about is a package with no boundary.
 *
 * Those packages hold no JS today, so both rules below pass over them in
 * silence. That is the correct amount of work for a declaration — but it means
 * the first C package that grows a script is the first one either rule has
 * anything to say about, and rule 2 in particular will have to be revisited
 * then, since `krudd/` internals being private to @kruddage/engine reads
 * differently once packages live inside `krudd/` (#920).
 */
export function packageDirs(repo) {
	return [
		...readdirSync(join(repo, "packages")).map((d) => join("packages", d)),
		"tools/render-diff",
		...manifestDirs(repo, "krudd/engine"),
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
