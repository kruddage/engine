// SPDX-License-Identifier: GPL-2.0-or-later
//
// Where @kruddage/engine reaches the build.
//
// It resolves @kruddage/kruddmake — a declared dependency in this package's
// manifest — rather than spelling a path to the repo root. That is the whole
// point of #920: "only @kruddage/engine may drive the engine build" used to be
// a regex in scripts/check-barriers.mjs matching the string "krudd.sh", and is
// now an edge in the package graph. A package that has not declared the
// dependency cannot resolve it.
//
// The resolution goes through "./package.json", which is the only thing
// kruddmake exports. There is no JS surface to import — kruddmake is Scheme
// behind a POSIX shell entry point, and this module spawns that entry point.

import { spawnSync } from "node:child_process";
import { createRequire } from "node:module";
import { dirname, join } from "node:path";

const require = createRequire(import.meta.url);

/** The kruddmake package directory. */
export const KRUDDMAKE = dirname(
	require.resolve("@kruddage/kruddmake/package.json")
);

/** Its entry point — the surface named in krudd/kruddmake/README.md. */
export const KRUDDMAKE_SH = join(KRUDDMAKE, "kruddmake.sh");

/** The whole native suite: kruddmake's Scheme tests, then the toolchain ones. */
export const KRUDDMAKE_TESTS = join(KRUDDMAKE, "run-tests.sh");

/**
 * Run one of kruddmake's shell entry points, inheriting stdio.
 *
 * Invoked through `sh` rather than executed directly: pnpm does not preserve
 * the executable bit when it materializes a workspace link on every platform,
 * and these scripts are POSIX shell by design (no node in the native suite's
 * path).
 *
 * @param {string} script absolute path to a kruddmake entry point
 * @param {string[]} args arguments for it
 * @param {{cwd: string, env?: Record<string, string>}} options
 * @returns {number} exit status, or 1 if it died on a signal
 */
export function runKruddmake(script, args, { cwd, env = {} }) {
	const result = spawnSync("sh", [script, ...args], {
		cwd,
		stdio: "inherit",
		env: { ...process.env, ...env },
	});

	return result.status ?? 1;
}
