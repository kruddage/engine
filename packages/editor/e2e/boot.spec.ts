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
//
// ## Why the boot test asks for WebGL
//
// The engine defaults to WebGPU, and the runner this executes on is headless
// Chromium with no GPU. `?renderer=webgl` is the shell's own documented opt-out
// and the editor implements the same rule, so this is selecting a supported
// path rather than working around a defect.
//
// The claim being tested is "the module boots and reports a live renderer".
// WebGL is a live renderer. Pinning the backend also makes the test deterministic
// about *which* path it exercises, which the earlier version was not — it
// assumed a fallback that the engine does not actually perform.

import { expect, test } from "@playwright/test";
import type { Page } from "@playwright/test";

/**
 * Collect everything the page says about itself.
 *
 * A boot that stalls reports `loading…` and nothing else, which names the
 * symptom and hides the cause — the first CI run of this file failed three
 * times over 45s each and produced no evidence at all. Attaching the page's
 * errors, its console and the engine's own stdout to the failure is what turns
 * the next stall into one run instead of three.
 */
function collect(page: Page): { report: () => string } {
	const errors: string[] = [];
	const logs: string[] = [];

	page.on("pageerror", (error) => errors.push(`${error.name}: ${error.message}`));
	page.on("console", (message) => logs.push(`[${message.type()}] ${message.text()}`));

	return {
		report: () =>
			[
				`page errors:\n${errors.length > 0 ? errors.join("\n") : "  (none)"}`,
				`console (last 40):\n${
					logs.length > 0 ? logs.slice(-40).join("\n") : "  (none)"
				}`,
			].join("\n\n"),
	};
}

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
		const { report } = collect(page);

		await page.goto("/?renderer=webgl");

		const phase = page.getByTestId("status-phase");

		/* Reaching "running" means main() ran inside the WASM module and called
		 * back into the page — krudd_signal_running() is the line immediately
		 * after engine_init() returns. That is the whole claim. */
		try {
			await expect(phase).toHaveText(/running|ready/, { timeout: 45_000 });
		} catch {
			const [last, engineLog] = await Promise.all([
				phase.textContent(),
				page
					.getByTestId("engine-log")
					.textContent()
					.catch(() => null),
			]);

			throw new Error(
				`the engine never reached "running" — last phase was ${JSON.stringify(last)}.\n\n` +
					`Neither kruddSetRunning nor an abort fired, so engine_init() did ` +
					`not return.\n\n${report()}\n\n` +
					`engine stdout/stderr:\n${engineLog ?? "  (the log panel rendered nothing)"}`
			);
		}

		await expect(phase).toHaveText(/running|ready/);

		/* Which backend is pinned by the query above, so this asserts the badge
		 * was actually overwritten by the engine rather than left at its seed. */
		await expect(page.getByTestId("status-renderer")).not.toHaveText(
			"renderer — booting…"
		);
	});
});
