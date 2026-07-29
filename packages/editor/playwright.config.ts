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

/* One address, used by both the server and the client that waits for it. See
 * the note on webServer.command below for why this is not simply "localhost". */
const HOST = "127.0.0.1";

/**
 * The prefix the build is served under, and it is not "/" on purpose.
 *
 * Every harness this package had served the editor at the domain root, and a
 * deploy never does — GitHub Pages puts the site under `/engine/`, and a PR
 * preview under `/engine/pr-preview/pr-<N>/` on top of that. That gap hid a
 * real bug for as long as it existed: the engine's base was absolute, which is
 * indistinguishable from relative at "/" and asks the wrong origin-relative
 * path everywhere else (see boot.ts, "Where the loader comes from").
 *
 * Two segments rather than one, so a fix that reaches for a single `../` is
 * still wrong here. The value is arbitrary; being non-empty is the test.
 */
const PREFIX = "/site/preview/";

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
		/* Trailing slash included, and load-bearing: Playwright resolves a
		 * test's URL against this the way the browser would, so dropping it
		 * would make "?renderer=webgl" resolve against the parent directory.
		 * For the same reason no test below may navigate to a path starting
		 * with "/" — that would climb back out to the root and stop testing
		 * what this prefix exists to test. */
		baseURL: `http://${HOST}:${PORT}${PREFIX}`,
		trace: "on-first-retry",
		video: "retain-on-failure",
	},
	projects: [
		{
			name: "chromium",
			use: {
				...devices["Desktop Chrome"],
				launchOptions: {
					/* A CI runner has no GPU, and the engine needs a real
					 * rendering context rather than a stub — this suite exists
					 * to boot it, not to watch it fail to find one. These route
					 * WebGL through SwiftShader's software rasterizer; recent
					 * Chrome requires the second flag to allow that fallback at
					 * all rather than refusing the context outright.
					 *
					 * They do not enable WebGPU. e2e/boot.spec.ts asks for
					 * WebGL explicitly, which is the shell's own documented
					 * opt-out, so nothing here depends on a GPU existing. */
					args: ["--use-angle=swiftshader", "--enable-unsafe-swiftshader"],
				},
			},
		},
	],
	webServer: {
		/* --host is explicit, and it is load-bearing rather than tidiness.
		 * Without it `vite preview` binds whatever `localhost` resolves to,
		 * which is 127.0.0.1 on a typical dev box and ::1 inside the emsdk
		 * container CI runs this job in. Playwright then polls the IPv4 address
		 * below, nothing is listening on it, and the run dies after 60s of
		 * waiting on a server that started fine — a failure that reproduces
		 * only in CI and blames the wrong thing when it does. Naming one
		 * address in both places is what makes them agree. */
		/* --base mounts the same build under PREFIX. It needs no rebuild: the
		 * production `base` is "./", so every URL in the output is relative to
		 * wherever the document lands. That is exactly the property this is
		 * here to keep honest. */
		command: `pnpm exec vite preview --host ${HOST} --port ${PORT} --strictPort --base ${PREFIX}`,
		url: `http://${HOST}:${PORT}${PREFIX}`,
		reuseExistingServer: !process.env["CI"],
		stdout: "pipe",
		stderr: "pipe",
	},
});
