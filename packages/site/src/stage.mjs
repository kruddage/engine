// SPDX-License-Identifier: GPL-2.0-or-later
//
// Stages the deployable site from a set of engine artifacts.
//
// This is the logic .github/scripts/stage-site.sh used to carry, moved into
// something testable and given the check the shell version could not make.
//
// The whitelist is unchanged in spirit: the kruddmake build directory holds
// intermediate objects and archives that must never reach the Pages branch, so
// only declared artifacts are copied. What is new is that the whitelist is no
// longer written here — it comes from @kruddage/engine, which owns the question
// of what it publishes and which of its outputs may be renamed.
//
// The rename itself is the fragile part. index.wasm is served under a hashed
// name, and the page finds it only because the shell template baked the same
// hash into a Module.locateFile hook at build time. Two independent derivations
// of one hash, in two languages, that must agree or the deploy 404s in the
// browser and nowhere else. staging now takes the hash *from the built HTML*
// and cross-checks it, so a mismatch is a build error rather than a broken site.

import {
	cpSync,
	existsSync,
	mkdirSync,
	readFileSync,
	rmSync,
	writeFileSync,
} from "node:fs";
import { join } from "node:path";

import { ENGINE_PROJECT_INDEX, bustedName } from "@kruddage/engine";

/**
 * Replace every occurrence of `from` with `to`, requiring at least one.
 *
 * stage-site.sh used `sed s/index\.js/.../`, which succeeds silently when the
 * pattern is absent — so a shell template that stopped emitting the loader tag
 * would have staged an index.html pointing at a file that was never written.
 */
function rewrite(text, from, to, context) {
	const parts = text.split(from);
	if (parts.length === 1) {
		throw new Error(
			`${context}: expected a reference to ${from} to rewrite, found none`
		);
	}
	return parts.join(to);
}

/**
 * Plan the staged layout: what each artifact is called on the site.
 *
 * Pure — no filesystem access — so the naming and rewriting rules can be tested
 * without a build. `stem` is the cache-busting hash; artifacts that declare
 * cacheBusting: false keep their names.
 */
export function planSite(artifacts, stem) {
	if (!stem) throw new Error("planSite: a cache-busting stem is required");

	const entries = artifacts.map((artifact) => ({
		artifact,
		from: artifact.name,
		to: bustedName(artifact, stem),
	}));

	const entry = entries.filter((e) => e.artifact.role === "entry");
	if (entry.length !== 1) {
		throw new Error(
			`planSite: expected exactly one entry artifact, got ${entry.length}`
		);
	}

	return {
		entry: entry[0],
		entries,
		/* Not simply "everything that was renamed" — index.wasm is renamed but
		 * resolved at runtime by Module.locateFile, and its only literal
		 * mention in the HTML is the error overlay's display text. The engine
		 * says which renames the document actually carries. */
		rewrites: entries.filter(
			(e) => e.artifact.rewriteInEntry && e.from !== e.to
		),
	};
}

/**
 * Apply a plan to disk.
 *
 * Returns the staged filenames, in the order they were written.
 */
export function stageSite({ artifacts, outDir, stem, assetDir = null }) {
	const plan = planSite(artifacts, stem);

	rmSync(outDir, { recursive: true, force: true });
	mkdirSync(outDir, { recursive: true });

	const staged = [];
	for (const { artifact, to } of plan.entries) {
		/* The entry document is rewritten rather than copied — its references
		 * to the renamed files have to move with them. */
		if (artifact.role === "entry") {
			let html = readFileSync(artifact.path, "utf8");
			for (const r of plan.rewrites) {
				html = rewrite(html, r.from, r.to, "index.html");
			}
			writeFileSync(join(outDir, to), html);
		} else {
			cpSync(artifact.path, join(outDir, to));
		}
		staged.push(to);
	}

	if (assetDir) {
		cpSync(assetDir, join(outDir, "assets"), { recursive: true });
		staged.push("assets/");
		checkProjectIndex(join(outDir, "assets"));
	}

	return staged;
}

/**
 * Check the staged asset directory against the project index inside it.
 *
 * The index is what the page asks for first and every project it lists is a URL
 * the page will then request, so an index naming a file that was not staged is
 * a 404 the user meets by clicking the one control this directory exists for —
 * and, like the wasm rename before it, a failure that would otherwise surface
 * only in a browser on the deployed site. Cheap to check here, where the whole
 * staged tree is on disk: this is the one thing about assets/ that staging can
 * be wrong about on its own.
 *
 * A directory with no index is fine — a build that staged no project has
 * nothing to serve and nothing to promise.
 */
function checkProjectIndex(dir) {
	const path = join(dir, ENGINE_PROJECT_INDEX);
	if (!existsSync(path)) return;

	let names;
	try {
		names = JSON.parse(readFileSync(path, "utf8"));
	} catch (e) {
		throw new Error(`${ENGINE_PROJECT_INDEX} is not valid JSON: ${e.message}`);
	}
	if (!Array.isArray(names)) {
		throw new Error(`${ENGINE_PROJECT_INDEX} must be an array of filenames`);
	}

	const missing = names.filter((name) => !existsSync(join(dir, name)));
	if (missing.length > 0) {
		throw new Error(
			`${ENGINE_PROJECT_INDEX} names ${missing.join(", ")}, ` +
				`which the build did not stage into assets/`
		);
	}
}
