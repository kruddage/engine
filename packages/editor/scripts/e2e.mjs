// SPDX-License-Identifier: GPL-2.0-or-later
//
// Runs the browser suite, and refuses to pretend when it cannot.
//
// The obvious alternative — a `test.skip()` when the engine is missing — is the
// thing this file exists to avoid. A skipped browser suite reports green, and a
// green suite that never booted a module is exactly the outcome #946 warns
// about: "a package that builds but has never touched wasm has not proven the
// thing it exists to prove."
//
// So: if you ask for the browser suite and the engine is not there, that is a
// failure with the one command that fixes it, not a pass with a note.

import { spawnSync } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { isBuilt } from "@kruddage/engine";

const HERE = dirname(fileURLToPath(import.meta.url));
const PACKAGE = resolve(HERE, "..");

const BUILD_COMMAND = "pnpm --filter @kruddage/engine run build";

if (!isBuilt()) {
	process.stderr.write(
		"\n@kruddage/editor: the browser suite needs a built engine, and there " +
			"is none.\n\n" +
			`  ${BUILD_COMMAND}\n\n` +
			"That needs emsdk on PATH. If you are working on the editor's chrome " +
			"and do not have it,\nthe component suite is the one to run:\n\n" +
			"  pnpm --filter @kruddage/editor run test\n\n"
	);
	process.exit(1);
}

/* Build first. playwright.config.ts serves the production output through `vite
 * preview` rather than the dev server, so there has to be one. */
const steps = [
	["pnpm", ["exec", "vite", "build"]],
	["pnpm", ["exec", "playwright", "test", ...process.argv.slice(2)]],
];

for (const [command, args] of steps) {
	const result = spawnSync(command, args, { cwd: PACKAGE, stdio: "inherit" });
	if (result.status !== 0) process.exit(result.status ?? 1);
}
