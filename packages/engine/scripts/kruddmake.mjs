// SPDX-License-Identifier: GPL-2.0-or-later
//
// Where @kruddage/engine reaches the build.
//
// kruddmake is not a package (#934), so there is nothing to resolve: the paths
// below are spelled out from the repo root, which this file locates by counting
// back from its own — packages/engine/scripts, three levels down.
//
// That is a package reaching krudd/ by path, which scripts/check-barriers.mjs
// rule 3 forbids. What permits it is the exemption written into that rule: it
// applies to every package but @kruddage/engine, and this is @kruddage/engine.
// The exemption is the whole of the permission, and since rule 2 went with the
// package it named, it is also the only gate between krudd/ and the rest of the
// workspace — so the reason to keep this the one file that knows kruddmake's
// layout has changed. It is no longer tidiness. Every other module in this
// package asks here, which keeps the paths that rule 3 would otherwise catch
// countable on sight.
//
// There was never a JS surface to import in any case. kruddmake is Scheme
// behind a POSIX shell entry point, and this module spawns that entry point.

import { spawnSync } from "node:child_process";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

/* packages/engine/scripts -> packages/engine -> packages -> the repo root. */
const REPO = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");

/** The build entry point — the surface named in krudd/kruddmake/README.md. */
export const KRUDDMAKE_SH = join(REPO, "krudd/kruddmake/kruddmake.sh");

/** The whole native suite: kruddmake's Scheme tests, then the toolchain ones. */
export const KRUDDMAKE_TESTS = join(REPO, "krudd/kruddmake/run-tests.sh");

/**
 * Run one of kruddmake's shell entry points, inheriting stdio.
 *
 * Invoked through `sh` rather than executed directly. These scripts are POSIX
 * shell by design — no node anywhere in the native suite's path — and `sh
 * <script>` is how CI, the README and a contributor with a compiler all invoke
 * them, so it is the form that gets exercised.
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
