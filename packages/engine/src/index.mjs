// SPDX-License-Identifier: GPL-2.0-or-later
//
// @kruddage/engine — the engine's JS-facing API.
//
// Everything downstream reads the build through this module. Nothing outside
// this package may open krudd/ by path or reach into the kruddmake build
// directory: driving the build is this package's private business, and
// scripts/check-barriers.mjs fails the workspace if another package does
// either.
//
// What is offered is deliberately narrow — where the artifacts are, what role
// each one plays, and which C entry points the module exports. The engine is
// one emscripten link unit, so there is no finer JS seam to hand out honestly
// (see README.md, "What this package is not").

import { existsSync, readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import {
	ENGINE_ARTIFACTS,
	ENGINE_ASSET_DIR,
	ENGINE_EXPORTED_FUNCTIONS,
	ENGINE_MANIFEST_FILE,
	bustedName,
} from "./artifacts.mjs";

export {
	ENGINE_ARTIFACTS,
	ENGINE_ASSET_DIR,
	ENGINE_EXPORTED_FUNCTIONS,
	bustedName,
};

const HERE = dirname(fileURLToPath(import.meta.url));

/** Absolute path to the built artifacts. */
export const distDir = resolve(HERE, "..", "dist");

/**
 * The commit hash the shell template baked into its Module.locateFile hook.
 *
 * The deploy step must rename index.wasm to exactly this stem or the page
 * requests a URL that was never staged. Reading it back out of the built HTML
 * — rather than re-deriving it from git — is what makes the two sides provably
 * agree instead of merely agreeing by construction.
 */
export function bakedCacheStem(html) {
	const match = html.match(/\.replace\(\/\\\.wasm\$\/,\s*'\.([^.']+)\.wasm'\)/);
	return match ? match[1] : null;
}

function manifestPath() {
	return join(distDir, ENGINE_MANIFEST_FILE);
}

/**
 * Read the manifest written by this package's build.
 *
 * Throws with the command to run rather than returning something empty: a
 * consumer that has not built yet is a sequencing mistake, and pnpm's
 * topological ordering means it should not be reachable from `pnpm build`.
 */
export function readManifest() {
	const path = manifestPath();
	if (!existsSync(path)) {
		throw new Error(
			`@kruddage/engine has not been built (no ${ENGINE_MANIFEST_FILE} in ${distDir}).\n` +
				`Run: pnpm --filter @kruddage/engine run build`
		);
	}
	return JSON.parse(readFileSync(path, "utf8"));
}

/** Whether a build output is present. */
export function isBuilt() {
	return existsSync(manifestPath());
}

/**
 * The built artifacts, each with its absolute path resolved.
 *
 * Returns the manifest's file list rather than ENGINE_ARTIFACTS so a build that
 * emitted extra outputs (a second .wasm, say) is described as it actually is.
 */
export function artifacts() {
	const manifest = readManifest();
	return manifest.files.map((file) => ({
		...file,
		path: join(distDir, file.name),
	}));
}

/** Absolute path to one artifact, by its build-output name. */
export function artifactPath(name) {
	const found = artifacts().find((a) => a.name === name);
	if (!found) {
		throw new Error(
			`@kruddage/engine exposes no artifact named ${name} ` +
				`(have: ${artifacts()
					.map((a) => a.name)
					.join(", ")})`
		);
	}
	return found.path;
}

/** Absolute path to the runtime asset directory, or null when the build made none. */
export function assetDir() {
	const path = join(distDir, ENGINE_ASSET_DIR);
	return existsSync(path) ? path : null;
}
