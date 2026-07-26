// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Finding and launching Chromium, and handing back the one page's own
 * DevTools Protocol endpoint.
 *
 * Headful, always — see `render-test.ts`'s header for why `--headless` is
 * refused on principle, not merely unused. `--no-sandbox` is unrelated to
 * that: it is a container/CI concern (Chromium refuses to run its own
 * sandbox as root, which is how this harness's own dev environment and many
 * CI runners are set up), not a rendering-mode one.
 */

import { type ChildProcess, spawn } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

/** A running Chromium, and the one page it opened. */
export interface Browser {
	readonly process: ChildProcess;
	readonly webSocketDebuggerUrl: string;
	close(): void;
}

/** Environment variables checked for an explicit Chromium path, in order. */
const PATH_ENV_VARS = ["KRUDD_CHROMIUM", "CHROME_PATH"];

/**
 * Conventional install locations checked when no environment variable names
 * one.
 *
 * The first is where this repository's own dev sandbox keeps a
 * Playwright-fetched Chromium (`PLAYWRIGHT_BROWSERS_PATH=/opt/pw-browsers`),
 * present without `playwright` ever being a dependency of this project —
 * nothing here imports the `playwright` package, the browser binary just
 * happens to already be on disk at that path in this environment. CI takes a
 * different route (`browser-actions/setup-chrome` in `ci.yml`) and points
 * `KRUDD_CHROMIUM` at the result, so neither this function nor the CI step
 * depends on the other's approach.
 */
const CONVENTIONAL_PATHS = [
	"/opt/pw-browsers/chromium",
	"/usr/bin/chromium",
	"/usr/bin/chromium-browser",
	"/usr/bin/google-chrome",
];

/** Locates a Chromium executable, or throws with where it looked. */
export function findChromium(): string {
	for (const name of PATH_ENV_VARS) {
		const value = process.env[name];
		if (value !== undefined && value !== "") {
			return value;
		}
	}
	for (const path of CONVENTIONAL_PATHS) {
		if (existsSync(path)) {
			return path;
		}
	}
	throw new Error(
		"no Chromium executable found — set KRUDD_CHROMIUM or CHROME_PATH to " +
			`one, or install it at one of: ${CONVENTIONAL_PATHS.join(", ")}`,
	);
}

/** How long to wait for Chromium to open its DevTools port before giving up. */
const LAUNCH_TIMEOUT_MS = 20_000;

/**
 * Launches Chromium against `about:blank` and returns its one page's CDP
 * endpoint.
 *
 * `about:blank` rather than the real page: the init script that makes the
 * page's clock steppable has to be registered over CDP *before* the real
 * navigation happens, or the page's own first frame runs on the real clock
 * and everything downstream stops being deterministic. See
 * `render-test.ts`, which navigates only after `Page.addScriptToEvaluate-
 * OnNewDocument` is in place.
 */
export async function launch(executablePath: string): Promise<Browser> {
	const userDataDir = mkdtempSync(join(tmpdir(), "krudd-render-test-"));
	const stderr: Buffer[] = [];
	const child = spawn(
		executablePath,
		[
			`--user-data-dir=${userDataDir}`,
			"--remote-debugging-port=0",
			"--no-first-run",
			"--no-default-browser-check",
			"--no-sandbox",
			"--disable-dev-shm-usage",
			"--window-size=800,600",
			"--force-device-scale-factor=1",
			// Deterministic software rendering: the same rasteriser regardless
			// of what GPU (or lack of one) the host has. Real GPU drivers vary
			// pixel rounding across vendors and driver versions; SwiftShader,
			// run the same way on every machine, does not — which is the whole
			// point of a checked-in reference image.
			"--use-gl=angle",
			"--use-angle=swiftshader",
			"--enable-unsafe-swiftshader",
			// A window with no manager to focus it (which is what Xvfb gives
			// every window) is exactly the case Chromium's power-saving
			// heuristics throttle hardest. Without these three, the stepped
			// virtual clock in `inject.ts` can end up racing a real
			// `requestAnimationFrame` that Chromium itself decided to skip.
			"--disable-background-timer-throttling",
			"--disable-backgrounding-occluded-windows",
			"--disable-renderer-backgrounding",
			"about:blank",
		],
		{ stdio: ["ignore", "ignore", "pipe"] },
	);
	child.stderr?.on("data", (chunk: Buffer) => stderr.push(chunk));

	let exited = false;
	child.once("exit", () => {
		exited = true;
	});

	const port = await waitForDevToolsPort(
		userDataDir,
		child,
		() => exited,
		stderr,
	);
	const response = await fetch(`http://127.0.0.1:${port}/json/list`);
	const targets = (await response.json()) as Array<{
		type: string;
		webSocketDebuggerUrl: string;
	}>;
	const page = targets.find((target) => target.type === "page");
	if (page === undefined) {
		throw new Error("Chromium opened no page target to attach to");
	}

	return {
		process: child,
		webSocketDebuggerUrl: page.webSocketDebuggerUrl,
		close(): void {
			child.kill("SIGKILL");
		},
	};
}

/**
 * Polls for the `DevToolsActivePort` file Chromium writes into its
 * `--user-data-dir` once its DevTools server is listening — the standard way
 * to discover an OS-assigned `--remote-debugging-port=0` without scraping
 * stdout.
 */
async function waitForDevToolsPort(
	userDataDir: string,
	child: ChildProcess,
	hasExited: () => boolean,
	stderr: Buffer[],
): Promise<number> {
	const portFile = join(userDataDir, "DevToolsActivePort");
	const deadline = Date.now() + LAUNCH_TIMEOUT_MS;
	while (Date.now() < deadline) {
		if (hasExited()) {
			throw new Error(
				`Chromium exited before opening its DevTools port:\n${Buffer.concat(stderr).toString("utf8")}`,
			);
		}
		if (existsSync(portFile)) {
			const firstLine = readFileSync(portFile, "utf8").split("\n")[0];
			const port = Number(firstLine);
			if (Number.isInteger(port) && port > 0) {
				return port;
			}
		}
		await sleep(50);
	}
	child.kill("SIGKILL");
	throw new Error(
		`Chromium did not open its DevTools port within ${LAUNCH_TIMEOUT_MS}ms:\n${Buffer.concat(stderr).toString("utf8")}`,
	);
}

function sleep(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}
