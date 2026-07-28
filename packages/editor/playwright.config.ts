// SPDX-License-Identifier: GPL-2.0-or-later
//
// The browser suite (#944, Q6).
//
// It runs against `vite preview` over a production build, not against the dev
// server. The distinction is the point: preview serves the engine's artifacts
// as the static files the plugin copied into the output, which is what a deploy
// actually serves. The dev server serves them through our own middleware, and a
// suite that only ever exercised that path would not have tested the thing that
// ships.
//
// This suite is not run by `pnpm test`. It needs a built engine, and CI's
// workspace job has no emsdk — see scripts/e2e.mjs, which refuses to run rather
// than skipping quietly.

import { defineConfig, devices } from "@playwright/test";

const PORT = 4173;

export default defineConfig({
	testDir: "./e2e",
	/* A WASM engine booting a renderer is not a 5-second operation on a cold CI
	 * runner, and the failure mode of guessing low is a flaky suite that gets
	 * ignored. */
	timeout: 60_000,
	expect: { timeout: 15_000 },
	fullyParallel: true,
	forbidOnly: !!process.env["CI"],
	retries: process.env["CI"] ? 2 : 0,
	reporter: process.env["CI"] ? [["github"], ["list"]] : [["list"]],
	use: {
		baseURL: `http://127.0.0.1:${PORT}`,
		trace: "on-first-retry",
		video: "retain-on-failure",
	},
	projects: [
		{ name: "chromium", use: { ...devices["Desktop Chrome"] } },
	],
	webServer: {
		command: `pnpm exec vite preview --port ${PORT} --strictPort`,
		url: `http://127.0.0.1:${PORT}`,
		reuseExistingServer: !process.env["CI"],
		stdout: "pipe",
		stderr: "pipe",
	},
});
