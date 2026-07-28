// SPDX-License-Identifier: GPL-2.0-or-later
//
// Workspace boundary check.
//
// The reason for splitting this repo into packages is to get barriers that hold
// on their own, rather than by everyone remembering where the lines are. This
// is what makes them hold.
//
// Two rules. They are numbered 1 and 3, and the gap is deliberate: these
// numbers are names. WORKSPACE.md's Q4 and the issues behind it argue about
// "rule 2" and "rule 3" by number, and sliding rule 3 down into the vacancy
// would leave every one of those references pointing at a rule it was not
// written about — silently, because the number it now names is also a real
// one. A gap a reader can see beats a citation that has quietly moved.
//
//   1. A package may not reach into another package by relative path. Cross-
//      package code travels through the package name, which means it travels
//      through the "exports" map, which means the importing side can only see
//      what the imported side chose to publish. A `../../engine/src/internal`
//      import routes around all of that, and it is the way every module system
//      erodes.
//
//   3. Nothing but @kruddage/engine may reach the build tree out of band —
//      by a path into krudd/, or through the generator's environment
//      (KRUDD_TARGET, KRUDD_BUILD_DIR). Rule 1 has an opinion about importing
//      across a package boundary; it cannot see a spawn of a path or an env
//      var read, which is what is left for a text match to do.
//
// Rule 2 was "only @kruddage/engine may depend on @kruddage/kruddmake", and it
// went with the package it was about (#934). Its ground was already rule 3's:
// the same `krudd/` subtree, matched by path, under the same @kruddage/engine
// exemption. So it goes rather than being reimplemented as a regex beside the
// one that already works — two mechanisms for one rule is how the second one
// rots (WORKSPACE.md, Q4). Rule 3 lost a pattern the same way and for the same
// reason: it used to match the repo-root forwarding script's filename, which
// was a package boundary drawn with a regex because the thing it protected had
// no package to be inside of. That script is retired (#935).
//
// The cost of collapsing onto rule 3 is real, and it is named again beside the
// rule itself: rule 3's @kruddage/engine exemption is now load-bearing where it
// used to be belt-and-braces.
//
// Run: pnpm check

import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

import { stripComments } from "./strip-comments.mjs";

const SKIP_DIRS = new Set(["node_modules", "dist", "out", "shots", ".git"]);

/* The extensions a rule can read. Getting this list wrong does not fail the
 * check — it silently shrinks it, which is the worse failure, so it is worth
 * being explicit about why each one is here.
 *
 * jsx and tsx arrived with @kruddage/editor (#946). Until then every package
 * here was plain Node and the list happened to be complete; a React package is
 * mostly .tsx, so leaving them out would have meant rule 1 reading the editor's
 * handful of .ts files and none of its components — a boundary check that
 * reports OK because it looked at almost nothing. That is the failure mode this
 * comment exists to prevent the next time the workspace grows a language.
 *
 * cts is here for symmetry with mts rather than because anything uses it. */
const SOURCE_EXT = /\.(mjs|cjs|js|jsx|mts|cts|ts|tsx)$/;

/* A relative specifier in an import/export/require. */
const RELATIVE_IMPORT =
	/(?:^|[^\w$])(?:import|export)[\s\S]{0,200}?from\s*["'](\.[^"']*)["']|(?:^|[^\w$])(?:import|require)\s*\(\s*["'](\.[^"']*)["']\s*\)/g;

/* The engine build reached out of band (rule 3).
 *
 * The @kruddage/engine exemption applied to these patterns — the `isEngine`
 * skip in findViolations — is load-bearing since #934 removed rule 2. It used
 * to be the second of two independent gates between a package and the build
 * tree: rule 2 caught the dependency edge, this caught the path. There is one
 * gate now. Widening the exemption, or letting a second package call itself
 * @kruddage/engine, leaves nothing behind it. */
const ENGINE_PRIVATE = [
	{
		pattern: /\bkrudd\/(kruddmake|engine|third_party)\//,
		what: "krudd/ by path",
	},
	{ pattern: /\bKRUDD_(TARGET|BUILD_DIR)\b/, what: "the kruddmake build environment" },
];

/**
 * Workspace package directories, relative to the repo root. Mirrors
 * pnpm-workspace.yaml, entry for entry: a package the boundary check does not
 * know about is a package with no boundary, so the two lists have to be read as
 * one thing. Both `packages/*` and `tools/*` are expanded off the filesystem
 * here for the same reason they are globs there — a new package under either
 * directory is inside the check the moment it exists, rather than when someone
 * remembers a second list.
 *
 * `tools/*` is read with manifestDirs rather than a plain readdir, because not
 * every directory under tools/ declares itself a package — tools/dawn-smoke is
 * C and a shell script, no package.json, no JS for rules 1 or 3 to read. A
 * plain readdir would hand its name to findViolations, which would then fail
 * it for the same reason it fails a ghost package: no readable package.json.
 * manifestDirs already answers "which directories here declared themselves a
 * package", which is exactly the question.
 *
 * krudd/ is not here and is not walked for manifests. The C tree left the
 * workspace with #934 and holds none; rules 1 and 3 read source files, and
 * there is no JS in that tree to read. Rule 3 guards it from the outside
 * instead, which is the only direction the guard was ever needed in.
 */
export function packageDirs(repo) {
	return [
		...readdirSync(join(repo, "packages")).map((d) => join("packages", d)),
		...(existsSync(join(repo, "tools")) ? manifestDirs(repo, "tools") : []),
	];
}

/**
 * Directories at or below `root` that hold a package.json, repo-relative.
 *
 * Used by packageDirs for tools/, where membership is discovered rather than
 * listed for the same reason packages/* is a glob: a package the boundary
 * check does not know about is a package with no boundary. It was unused
 * between #934 (which took the C tree's two manifests out of packageDirs) and
 * this commit (which gives tools/ the same discovery packages/ already had) —
 * kept across that gap because the question it answers is one a workspace
 * whose membership is discovered rather than listed always has to ask again.
 * Deleting it and writing it again was not a saving.
 */
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
	const repo = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
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
