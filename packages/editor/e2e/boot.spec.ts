// SPDX-License-Identifier: GPL-2.0-or-later
//
// The engine actually running, in an actual browser.
//
// This is the one test in the package that proves what #946 exists to prove:
// that the editor is not merely a React app that builds, but one that loads a
// real WASM module produced by kruddmake and gets a report back from it. Every
// other test in this package would pass against an engine that did not exist.
//
// Kept deliberately small. The rule from the Q6 decision is that a test lives
// here only when it would lie in Vitest; everything about the chrome that jsdom
// can answer belongs in the component suite, and a browser suite that grows to
// cover it is how a fast suite becomes one nobody runs.

import { expect, test } from "@playwright/test";

test.describe("engine boot", () => {
	test("serves the WASM module as application/wasm", async ({ request }) => {
		/* A browser refuses to streaming-compile a module served as anything
		 * else, and getting this wrong fails at runtime and nowhere else. The
		 * dev server sets the type explicitly; here it comes from whatever
		 * serves the built output, which is what a deploy will do. */
		const response = await request.get("/engine/index.wasm");

		expect(response.status()).toBe(200);
		expect(response.headers()["content-type"]).toBe("application/wasm");
	});

	test("reports the engine's identity from the real manifest", async ({ page }) => {
		await page.goto("/");

		/* Not a fixed string: the version comes from version.txt through the C
		 * build, and pinning it here would mean a release had to remember to
		 * update a browser test. What matters is that it is a version and that
		 * it came from the artifact rather than from a default. */
		await expect(page.getByTestId("engine-version")).toHaveText(
			/^\d+\.\d+\.\d+/
		);
		await expect(page.getByTestId("engine-unbuilt")).toHaveCount(0);
	});

	test("boots the module and reports a live renderer", async ({ page }) => {
		const errors: string[] = [];
		page.on("pageerror", (error) => errors.push(error.message));

		await page.goto("/");

		/* The engine's own callbacks drive this. Reaching "running" means main()
		 * ran inside the WASM module and called back into the page — which is
		 * the whole claim. */
		await expect(page.getByTestId("status-phase")).toHaveText(
			/running|ready/,
			{ timeout: 45_000 }
		);

		/* Which backend went live is the machine's business — a headless CI
		 * runner may have no WebGPU adapter and fall back to WebGL. That it
		 * reported *something* other than the seed is the assertion. */
		await expect(page.getByTestId("status-renderer")).not.toHaveText(
			"renderer — booting…"
		);

		expect(errors).toEqual([]);
	});
});
