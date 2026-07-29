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

import { cpSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";

import { bustedName } from "@kruddage/engine";

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
 * Where the engine's own artifacts live on the staged site.
 *
 * #946 put the editor at `editor/` and left the engine's shell at the root,
 * because at that point the editor was a skeleton and the shell was the only
 * editor that existed. #953 reverses it: the editor is the site, and the shell
 * is the engine's game host at its own route.
 *
 * Every reference the engine's page makes to its siblings is relative — the
 * loader `<script>`, `manifest.webmanifest` (whose `start_url` and `scope` are
 * both "."), `sw.js`, the icons and the runtime assets — so the whole set moves
 * together by being copied into one subdirectory, and nothing inside it has to
 * learn its own URL. The service worker's scope narrows to this prefix with it,
 * which is correct: it caches the game host, and the editor is not its business.
 */
export const ENGINE_PREFIX = "game";

/**
 * Apply a plan to disk.
 *
 * Returns the staged paths, in the order they were written.
 *
 * `editorDir`, when given, is @kruddage/editor's production output, copied in
 * whole to the site root. It is a directory rather than an artifact list because
 * the editor is a bundler's output — hashed chunk names nobody declares in
 * advance — which is the opposite of the engine's case, where the whitelist
 * exists precisely because the build directory also holds objects and archives
 * that must never reach the Pages branch. A Vite `outDir` holds only what Vite
 * put there.
 *
 * The two can no longer collide: the engine stages under ENGINE_PREFIX and the
 * editor at the root, so `index.html` means the editor's and nothing overwrites
 * anything. That is also why the editor is copied last — if the invariant ever
 * broke, the failure would be the editor winning at the root, which is the
 * outcome a reader can actually see and report.
 */
export function stageSite({
	artifacts,
	outDir,
	stem,
	assetDir = null,
	editorDir = null,
}) {
	const plan = planSite(artifacts, stem);
	const engineDir = join(outDir, ENGINE_PREFIX);

	rmSync(outDir, { recursive: true, force: true });
	mkdirSync(engineDir, { recursive: true });

	const staged = [];
	for (const { artifact, to } of plan.entries) {
		/* The entry document is rewritten rather than copied — its references
		 * to the renamed files have to move with them. */
		if (artifact.role === "entry") {
			let html = readFileSync(artifact.path, "utf8");
			for (const r of plan.rewrites) {
				html = rewrite(html, r.from, r.to, "index.html");
			}
			writeFileSync(join(engineDir, to), html);
		} else {
			cpSync(artifact.path, join(engineDir, to));
		}
		staged.push(`${ENGINE_PREFIX}/${to}`);
	}

	if (assetDir) {
		cpSync(assetDir, join(engineDir, "assets"), { recursive: true });
		staged.push(`${ENGINE_PREFIX}/assets/`);
	}

	if (editorDir) {
		cpSync(editorDir, outDir, { recursive: true });
		staged.push("./");
	}

	return staged;
}
