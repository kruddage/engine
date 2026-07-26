// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The `xvfb-run` re-exec.
 *
 * Deliberately the *only* branch on display availability anywhere in this
 * harness, and there is no `--headless` flag for it to fall back to instead.
 * Headless Chrome composites a WebGPU canvas as blank while reporting a
 * live, drawing device — see the write-up in #827 and `docs/toolchain.md` —
 * and the equivalent trap here is quieter still: a screenshot mode that runs
 * without a display and silently renders nothing would be worse than a
 * harness that refuses to run at all. So when there is no display, this
 * re-execs the whole process under `xvfb-run`, which gives Chromium a real
 * (if virtual) display, and it is never given the option to go headless
 * instead.
 *
 * Nothing here guards against re-entering forever: `xvfb-run` sets `DISPLAY`
 * itself before it runs the wrapped command, so the second time this
 * function runs — inside the re-exec'd process — the check below is simply
 * false and it returns immediately.
 */

import { spawnSync } from "node:child_process";

/** Re-execs the current process under `xvfb-run` if there is no display. */
export function ensureDisplay(): void {
	if (process.env.DISPLAY !== undefined && process.env.DISPLAY !== "") {
		return;
	}
	console.log("render-test: no DISPLAY — re-executing under xvfb-run");
	const result = spawnSync(
		"xvfb-run",
		[
			"--auto-servernum",
			"--server-args=-screen 0 1024x768x24",
			process.execPath,
			...process.argv.slice(1),
		],
		{ stdio: "inherit" },
	);
	if (result.error !== undefined) {
		throw new Error(
			`could not run xvfb-run: ${result.error.message} — is it installed? ` +
				"the CI step installs it explicitly; see .github/workflows/ci.yml",
		);
	}
	// The re-exec'd process already printed whatever it had to say; this one
	// only needs to carry its exit code back out.
	process.exit(result.status ?? 1);
}
