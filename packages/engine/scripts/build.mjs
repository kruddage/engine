// SPDX-License-Identifier: GPL-2.0-or-later
//
// Builds @kruddage/engine.
//
// This does not reimplement the engine build. It drives @kruddage/kruddmake —
// the same generator CI has always run, reached now through this package's
// declared dependency on it rather than through a path to the script that
// used to live at the repo root (#920) — and then harvests the outputs into
// dist/ with a manifest describing them.
// kruddmake stays the build system for C and WASM; this is the wrapper that
// turns its output into a package with a boundary around it.
//
//   KRUDD_VERSION     stamped into the build (default: version.txt)
//   KRUDD_BUILD_DIR   kruddmake's output directory (default: <repo>/build)

import { spawnSync } from "node:child_process";
import {
	cpSync,
	existsSync,
	mkdirSync,
	readFileSync,
	readdirSync,
	rmSync,
	statSync,
	writeFileSync,
} from "node:fs";
import { createHash } from "node:crypto";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import {
	ENGINE_ARTIFACTS,
	ENGINE_ASSET_DIR,
	ENGINE_EXPORTED_FUNCTIONS,
	ENGINE_MANIFEST_FILE,
} from "../src/artifacts.mjs";
import { bakedCacheStem } from "../src/index.mjs";
import { readWasmExports } from "../src/wasm-exports.mjs";
import { KRUDDMAKE_SH, runKruddmake } from "./kruddmake.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const PKG = resolve(HERE, "..");
const REPO = resolve(PKG, "..", "..");
const DIST = join(PKG, "dist");

function fail(message) {
	process.stderr.write(`\n@kruddage/engine: ${message}\n\n`);
	process.exit(1);
}

function have(command) {
	return spawnSync(command, ["--version"], { stdio: "ignore" }).status === 0;
}

/* ------------------------------------------------------------- toolchain */

if (!have("emcc")) {
	fail(
		"emcc not found on PATH.\n" +
			"The engine's deliverable is a WASM module, so the Emscripten SDK is\n" +
			"required to build this package:\n" +
			"  https://emscripten.org/docs/getting_started/downloads.html\n" +
			"Then `source /path/to/emsdk/emsdk_env.sh` and re-run.\n" +
			"(The native test suite needs no emcc — `pnpm --filter @kruddage/engine test`.)"
	);
}

if (!have("ninja")) {
	fail("ninja not found on PATH — kruddmake drives ninja(1) directly.");
}

/* ----------------------------------------------------------------- build */

const version =
	process.env.KRUDD_VERSION?.trim() ||
	readFileSync(join(REPO, "version.txt"), "utf8").trim();

const buildDir = process.env.KRUDD_BUILD_DIR
	? resolve(process.env.KRUDD_BUILD_DIR)
	: join(REPO, "build");

process.stdout.write(`@kruddage/engine: building ${version} (wasm)\n`);

const status = runKruddmake(KRUDDMAKE_SH, ["build"], {
	cwd: REPO,
	env: {
		KRUDD_TARGET: "wasm",
		KRUDD_VERSION: version,
		KRUDD_BUILD_DIR: buildDir,
	},
});

if (status !== 0) {
	fail(`kruddmake build failed (exit ${status})`);
}

/* --------------------------------------------------------------- harvest */

/* Every output kruddmake declares, plus any additional .wasm the link emitted.
 * stage-site.sh globbed for extra .wasm rather than naming one; keeping that
 * here means a build that ever splits the module is staged, not silently
 * half-published. */
const declared = new Set(ENGINE_ARTIFACTS.map((a) => a.name));
const extraWasm = existsSync(buildDir)
	? readdirSync(buildDir)
			.filter((f) => f.endsWith(".wasm") && !declared.has(f))
			.map((name) => ({
				name,
				role: "code",
				cacheBusting: true,
				rewriteInEntry: false,
				required: false,
			}))
	: [];

const wanted = [...ENGINE_ARTIFACTS, ...extraWasm];

const missing = wanted
	.filter((a) => a.required && !existsSync(join(buildDir, a.name)))
	.map((a) => a.name);

if (missing.length > 0) {
	fail(
		`the build did not produce: ${missing.join(", ")}\n` +
			`Looked in ${buildDir}. If kruddmake's output names changed, update\n` +
			`packages/engine/src/artifacts.mjs to match.`
	);
}

rmSync(DIST, { recursive: true, force: true });
mkdirSync(DIST, { recursive: true });

const files = [];
for (const artifact of wanted) {
	const from = join(buildDir, artifact.name);
	if (!existsSync(from)) continue;

	const bytes = readFileSync(from);
	cpSync(from, join(DIST, artifact.name));

	/* Spread the artifact rather than restating its fields: the manifest is
	 * what consumers see, and a hand-written field list here silently drops
	 * whatever the contract grows next. `required` is the one field that stays
	 * behind — it describes the build, not the artifact. */
	const { required, ...published } = artifact;
	files.push({
		...published,
		bytes: bytes.length,
		sha256: createHash("sha256").update(bytes).digest("hex"),
	});
}

const assetsFrom = join(buildDir, ENGINE_ASSET_DIR);
const hasAssets = existsSync(assetsFrom) && statSync(assetsFrom).isDirectory();
if (hasAssets) {
	cpSync(assetsFrom, join(DIST, ENGINE_ASSET_DIR), { recursive: true });
}

/* ---------------------------------------------------------------- verify */

/* The deploy step renames index.wasm, and the only reason the page still finds
 * it is the Module.locateFile hook the shell template baked in at configure
 * time (shell/web/shell.html.in). If that stem is missing, every deploy 404s on
 * the WASM module — a failure that used to surface only in a browser, on the
 * live site. Catch it at build time instead. */
const html = readFileSync(join(DIST, "index.html"), "utf8");
const cacheStem = bakedCacheStem(html);

if (!cacheStem) {
	fail(
		"index.html carries no Module.locateFile cache-busting stem.\n" +
			"The deploy step renames index.wasm and relies on that hook to resolve\n" +
			"it; without the hook the deployed page cannot load the module.\n" +
			"Check the @GIT_COMMIT_HASH@ substitution in shell/web/shell.html.in."
	);
}

/* emcc substitutes {{{ SCRIPT }}} with the loader tag. The deploy step rewrites
 * that reference when it renames index.js, so its absence would make the
 * rewrite a silent no-op. */
if (!html.includes("index.js")) {
	fail(
		"index.html does not reference index.js — the emscripten shell template\n" +
			"did not receive its {{{ SCRIPT }}} substitution."
	);
}

/* The real export surface, read back out of the module. Recorded rather than
 * asserted against ENGINE_EXPORTED_FUNCTIONS: emscripten is free to rename a
 * C entry point on the way out (a `main` taking argv comes through as
 * __main_argc_argv), so the artifact is the authority on what the surface is
 * and the declared list is what the engine intends to offer. Consumers can
 * compare the two; see README.md. */
/* Buffer is a Uint8Array and the reader honours byteOffset, so this is read
 * in place rather than copied — the module is tens of megabytes. */
const wasmExports = readWasmExports(readFileSync(join(DIST, "index.wasm")))
	.filter((e) => e.kind === "function")
	.map((e) => e.name)
	.sort();

/* -------------------------------------------------------------- manifest */

writeFileSync(
	join(DIST, ENGINE_MANIFEST_FILE),
	JSON.stringify(
		{
			name: "@kruddage/engine",
			version,
			cacheStem,
			declaredExports: ENGINE_EXPORTED_FUNCTIONS,
			wasmExports,
			assetDir: hasAssets ? ENGINE_ASSET_DIR : null,
			files,
		},
		null,
		"\t"
	) + "\n"
);

const total = files.reduce((sum, f) => sum + f.bytes, 0);
process.stdout.write(
	`@kruddage/engine: staged ${files.length} artifacts ` +
		`(${(total / 1024 / 1024).toFixed(1)} MiB), cache stem ${cacheStem}\n`
);
