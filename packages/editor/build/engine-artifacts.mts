// SPDX-License-Identifier: GPL-2.0-or-later
//
// The Vite plugin that puts the engine in front of the editor.
//
// This file is the whole of "how the editor meets the engine", and the shape of
// it is deliberate: the editor *consumes* the WASM artifact and never produces
// it. kruddmake stays the only thing that compiles C, version.txt stays the only
// source of the version, and this plugin asks @kruddage/engine where the outputs
// are rather than knowing. Ask it, do not look — the workspace boundary check
// enforces the difference, and it is the reason @kruddage/engine is a package.
//
// Two jobs:
//
//   dev    serve the built artifacts under ENGINE_BASE, so `vite dev` runs the
//          editor against a real engine with fast refresh over the top.
//   build  copy the same files into the production output, so what ships is
//          what was tested.
//
// A third, smaller job: expose the manifest to the app as a virtual module, so
// the editor can render the engine's version and exports without a fetch and
// without a second copy of the artifact contract.
//
// It tolerates an unbuilt engine on purpose. `pnpm --filter @kruddage/engine run
// build` needs emsdk, which a contributor working on the editor's chrome may not
// have and does not need. Rather than failing, the plugin reports `built: false`
// and the app says so plainly on screen. What must not happen is pretending —
// see scripts/e2e.mjs, which refuses to run a browser suite against an engine
// that is not there.

import { createReadStream, cpSync, existsSync } from "node:fs";
import { extname, isAbsolute, join, normalize, resolve } from "node:path";

import { artifacts, distDir, isBuilt, readManifest } from "@kruddage/engine";
import type { Plugin, ResolvedConfig } from "vite";

import type { EngineInfo } from "../src/engine/types.js";

export type { EngineInfo };

/** Where the engine's artifacts are served and shipped, relative to the site. */
export const ENGINE_BASE = "/engine/";

/** The one command that fixes `built: false`. */
export const ENGINE_BUILD_COMMAND = "pnpm --filter @kruddage/engine run build";

const VIRTUAL_ID = "virtual:krudd-engine";
const RESOLVED_ID = "\0" + VIRTUAL_ID;

/* Enough of a content-type table for what the engine actually emits. Vite's own
 * static middleware never sees these files — they are copied, not imported — so
 * the few types below are ours to get right. WASM especially: a browser refuses
 * to streaming-compile a module served as anything else. */
const CONTENT_TYPES: Record<string, string> = {
	".html": "text/html; charset=utf-8",
	".js": "text/javascript; charset=utf-8",
	".wasm": "application/wasm",
	".webmanifest": "application/manifest+json",
	".png": "image/png",
	".data": "application/octet-stream",
};

/**
 * What the editor should be told about the engine right now.
 *
 * Exported for the plugin's own test: the interesting behaviour is the unbuilt
 * branch, and asserting it through a running Vite server would be a worse test
 * of the same thing.
 */
export function describeEngine(): EngineInfo {
	if (!isBuilt()) {
		return {
			built: false,
			version: null,
			exports: [],
			base: ENGINE_BASE,
			buildCommand: ENGINE_BUILD_COMMAND,
		};
	}

	const manifest = readManifest();
	return {
		built: true,
		version: manifest.version,
		exports: manifest.wasmExports,
		base: ENGINE_BASE,
		buildCommand: ENGINE_BUILD_COMMAND,
	};
}

/**
 * Resolve a request path under ENGINE_BASE to a file in the artifact directory.
 *
 * Returns null when the result would escape it. Traversal is rejected by
 * construction rather than by pattern-matching the input: a dev server binds a
 * port on a contributor's machine, which is not a threat model to wave away.
 *
 * Exported so the rejection is testable without a socket.
 */
export function resolveArtifactRequest(url: string): string | null {
	if (!url.startsWith(ENGINE_BASE)) return null;

	const raw = url.slice(ENGINE_BASE.length).split(/[?#]/)[0] ?? "";
	let name: string;
	try {
		name = decodeURIComponent(raw);
	} catch {
		/* A malformed percent-escape is a bad request, not a path. */
		return null;
	}
	if (name === "" || isAbsolute(name)) return null;

	const target = normalize(join(distDir, name));
	if (target !== distDir && !target.startsWith(distDir + "/")) return null;
	return target;
}

/**
 * Serve and ship @kruddage/engine's artifacts alongside the editor.
 *
 * @param outSubdir where the artifacts land inside the production output.
 *   Defaults to ENGINE_BASE without its slashes.
 */
export function engineArtifacts(outSubdir = "engine"): Plugin {
	let config: ResolvedConfig;

	return {
		name: "krudd:engine-artifacts",

		configResolved(resolved) {
			config = resolved;
		},

		resolveId(id) {
			return id === VIRTUAL_ID ? RESOLVED_ID : null;
		},

		load(id) {
			if (id !== RESOLVED_ID) return null;
			/* Evaluated per load rather than captured when the plugin is
			 * constructed, so a dev server started before the engine was built
			 * picks the artifacts up on reload instead of needing a restart. */
			return `export const engine = ${JSON.stringify(describeEngine())};\n`;
		},

		configureServer(server) {
			server.middlewares.use((req, res, next) => {
				const url = req.url ?? "";
				if (!url.startsWith(ENGINE_BASE)) return next();

				const target = resolveArtifactRequest(url);
				if (target === null) {
					res.statusCode = 403;
					res.end("forbidden\n");
					return;
				}

				if (!existsSync(target)) {
					res.statusCode = 404;
					res.end(
						isBuilt()
							? `no such engine artifact: ${url}\n`
							: `the engine is not built — run: ${ENGINE_BUILD_COMMAND}\n`
					);
					return;
				}

				const type = CONTENT_TYPES[extname(target)];
				if (type) res.setHeader("Content-Type", type);
				/* kruddmake rebuilds these out of band and the dev server has no
				 * way to hear about it. Never cache them. */
				res.setHeader("Cache-Control", "no-store");
				createReadStream(target).pipe(res);
			});
		},

		closeBundle() {
			if (!isBuilt()) {
				this.warn(
					`the engine is not built, so no artifacts were shipped and the ` +
						`editor will render its unbuilt notice. Run: ${ENGINE_BUILD_COMMAND}`
				);
				return;
			}

			const out = join(resolve(config.root, config.build.outDir), outSubdir);
			for (const artifact of artifacts()) {
				cpSync(artifact.path, join(out, artifact.name));
			}
		},
	};
}
