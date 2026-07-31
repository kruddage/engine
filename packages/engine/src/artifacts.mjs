// SPDX-License-Identifier: GPL-2.0-or-later
//
// The engine's published artifact contract.
//
// kruddmake decides what the WASM build *emits* (see krudd/kruddmake/ninja.scm,
// ninja-emit-wasm-module and the PWA copy edges below it). This file decides
// what the engine *offers* — which of those outputs a consumer may rely on, and
// the one piece of policy a consumer cannot derive on its own: whether a file
// may be renamed for cache-busting.
//
// That policy has to live here rather than in the deploy step because the
// engine is what makes it true. index.wasm is renamed at deploy time, and the
// only reason that works is that the shell template resolves it through a
// Module.locateFile hook (shell/web/shell.html.in) built from the same commit
// hash. sw.js and manifest.webmanifest must keep their names because the page
// and the browser reference them literally. A deploy script that reinvented
// this table would be guessing at invariants it does not own.

/**
 * Roles:
 *   entry   the HTML document the site serves, and the only file that carries
 *           references needing rewriting when its siblings are renamed.
 *   code    emscripten output — renamed per build so a browser never serves a
 *           stale mix of loader and module.
 *   static  fetched by literal name (PWA manifest, service worker, icons), so
 *           renaming any of them breaks the reference that names it.
 *
 * `rewriteInEntry` is separate from `cacheBusting` because the two renamed
 * files are found in different ways, and conflating them corrupts the page.
 * index.js is named by the <script> tag emcc substitutes into the shell, so the
 * entry document has to be rewritten when it moves. index.wasm is *not*: the
 * shell's Module.locateFile hook applies the stem at runtime. The only literal
 * "index.wasm" in the HTML is the error overlay's display text — rewriting it
 * would leave the page telling the user a filename that does not exist.
 */
export const ENGINE_ARTIFACTS = [
	{
		name: "index.html",
		role: "entry",
		cacheBusting: false,
		rewriteInEntry: false,
		required: true,
	},
	{
		name: "index.js",
		role: "code",
		cacheBusting: true,
		rewriteInEntry: true,
		required: true,
	},
	{
		name: "index.wasm",
		role: "code",
		cacheBusting: true,
		rewriteInEntry: false,
		required: true,
	},
	{
		name: "manifest.webmanifest",
		role: "static",
		cacheBusting: false,
		rewriteInEntry: false,
		required: true,
	},
	{
		name: "sw.js",
		role: "static",
		cacheBusting: false,
		rewriteInEntry: false,
		required: true,
	},
	{
		name: "icon-192.png",
		role: "static",
		cacheBusting: false,
		rewriteInEntry: false,
		required: true,
	},
	{
		name: "icon-512.png",
		role: "static",
		cacheBusting: false,
		rewriteInEntry: false,
		required: true,
	},
];

/* Runtime assets the asset plugin fetches by path. Copied wholesale when the
 * build produced any; absent from a build with no runtime assets, which is not
 * an error. */
export const ENGINE_ASSET_DIR = "assets";

/* The C entry points the WASM module exports to JS. Mirrors -sEXPORTED_FUNCTIONS
 * in ninja.scm's $mainflags; kept here so a consumer can assert against the
 * surface it codes to instead of reading the generated loader. Verified against
 * the real build output by scripts/build.mjs. */
export const ENGINE_EXPORTED_FUNCTIONS = ["_main", "_krudd_load_game"];

/* Name of the self-describing index written beside the artifacts. */
export const ENGINE_MANIFEST_FILE = "engine-manifest.json";

/**
 * Apply a cache-busting stem to an artifact name: "index.wasm" + "abc1234"
 * becomes "index.abc1234.wasm". Names without cacheBusting pass through
 * unchanged, so a caller can map this over every artifact uniformly.
 */
export function bustedName(artifact, stem) {
	if (!artifact.cacheBusting) return artifact.name;

	const dot = artifact.name.lastIndexOf(".");
	if (dot <= 0) {
		throw new Error(
			`cache-busting artifact ${artifact.name} has no extension to insert before`
		);
	}
	return `${artifact.name.slice(0, dot)}.${stem}${artifact.name.slice(dot)}`;
}
