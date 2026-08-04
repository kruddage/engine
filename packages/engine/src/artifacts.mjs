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

/* Runtime files the page fetches by path rather than linking in: every project
 * source this build ships (assets/*.scm) and the index naming them
 * (assets/projects.json), which is how the shell's picker offers a project
 * without the page carrying a filename. Copied wholesale when the build
 * produced any; absent from a build that shipped nothing, which is not an
 * error. */
export const ENGINE_ASSET_DIR = "assets";

/* The index inside ENGINE_ASSET_DIR: a JSON array of the project filenames
 * beside it. Written by the build from the (project-source ...) /
 * (staged-project ...) declarations in the C tree (krudd/kruddmake/ninja.scm)
 * and read by the shell, which cannot list a directory over HTTP and must not
 * carry a project's filename. Every project the build ships is listed, not only
 * the one the image embedded — being embedded only spares a project the fetch,
 * and says nothing about whether the page offers it. Named here because it is a
 * fact about the published layout, and the staging step checks the shipped
 * directory against it. */
export const ENGINE_PROJECT_INDEX = "projects.json";

/* The C entry points the WASM module exports to JS. Mirrors -sEXPORTED_FUNCTIONS
 * in ninja.scm's $mainflags; kept here so a consumer can assert against the
 * surface it codes to instead of reading the generated loader. Verified against
 * the real build output by scripts/build.mjs.
 *
 * Opening a project is most of it. There is no "load this scene" entry point
 * beside _krudd_load_project: the page opens a project the build shipped by
 * navigating to ?game=<name>, which the module reads at boot rather than being
 * called about, so the only thing the page calls in for is a source it holds
 * and the module does not.
 *
 * _malloc and _free are here for one reason and it is worth naming: that path
 * passes a variable-length string INTO the module, and every other JS bridge in
 * the tree passes one out. A buffer for it has to come from the module's own
 * allocator, so the allocator is part of the published surface. The engine's own
 * bridge (project_host.c) is the only intended caller, and it frees what it
 * allocates within the one call.
 *
 * _krudd_suspend_loop / _krudd_resume_loop / _krudd_driven_tick (engine.c,
 * #991) let a host that embeds this module take the frame loop over: pause the
 * rAF loop emscripten_set_main_loop installed, drive engine_tick itself one
 * call at a time, then hand the loop back. No code in this package calls them
 * — the page here still runs on the plain rAF loop end to end — but a host
 * with its own frame source, that hands out per-frame data only through its
 * own callback, needs the loop suspended for as long as it is driving, and
 * there is no other way to get it. */
export const ENGINE_EXPORTED_FUNCTIONS = [
	"_main",
	"_krudd_load_project",
	"_malloc",
	"_free",
	"_krudd_suspend_loop",
	"_krudd_resume_loop",
	"_krudd_driven_tick",
];

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
