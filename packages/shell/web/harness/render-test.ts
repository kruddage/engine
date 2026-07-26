// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * `render-test` — the screenshot-and-compare harness against the built
 * WebGL2 page.
 *
 * #827's whole brief: no mode may return the same result for "rendered
 * correctly" and "rendered nothing". Three things follow from that, and each
 * has its own file:
 *
 * - **Headful, always.** `xvfb.ts` re-execs under `xvfb-run` when there is
 *   no display. There is no `--headless` flag anywhere in this harness —
 *   headless Chrome composites a WebGPU canvas as blank while reporting a
 *   live device, and the equivalent risk for any capture path is that a
 *   convenient-looking mode quietly stops proving anything. See
 *   `docs/toolchain.md` and #827's own write-up.
 * - **The comparison asserts non-blank unconditionally.** `compare.ts`'s
 *   `judge` fails on a uniform-colour capture even when the reference is
 *   also uniform, rather than trusting the diff to notice — two identical
 *   blank images diff as a perfect match, which is precisely the failure
 *   this instrument exists to catch.
 * - **Browser-side errors reach this driver instead of dying in the page's
 *   console.** `Runtime.consoleAPICalled` and `Runtime.exceptionThrown` are
 *   native CDP events — `console.error` and an uncaught exception
 *   ("pageerror") need no page-side shim to reach here. A lost WebGL context
 *   does need one, because it neither throws nor logs on its own; `inject.ts`
 *   forwards it as a `console.error` from inside the page, so it arrives
 *   through the same channel as everything else. (The WebGPU
 *   `requestDevice` shim the original tool also had is out of scope — this
 *   engine has no WebGPU backend at all; see #812.)
 *
 * ## Determinism, not just non-blankness
 *
 * The demo draws eight *drifting* triangles (`packages/shell/web/src/index.ts`),
 * animated against `performance.now()`. A reference screenshot compared
 * against a real-time capture would differ by however much the two runs'
 * clocks happened to disagree — solving that with a looser tolerance would be
 * exactly "a tolerance chosen to make a flaky test pass", the thing #827
 * warns against. So `inject.ts` replaces the page's clock and
 * `requestAnimationFrame` with a virtual one this driver steps by hand:
 * `FRAME_COUNT` identical, driver-chosen ticks, then a screenshot. The
 * `CHANNEL_TOLERANCE` and `MAX_DIFF_FRACTION` that remain in `compare.ts` are
 * there only for rasteriser rounding noise, never for the animation.
 *
 * ## Dependency-free
 *
 * Node's built-in `WebSocket` is the whole DevTools Protocol transport
 * (`cdp.ts`), and PNG decoding happens inside the page, which is already a
 * PNG codec (`compare.ts`) — no `node_modules` entry for either. Chromium
 * itself is not fetched by this harness: it is expected to already be on
 * disk (`chromium.ts` looks in a few conventional places, and CI installs it
 * with a GitHub Action rather than a project dependency).
 *
 * Run by `cargo xtask render-test`, never directly — see
 * `xtask/src/render_test.rs`, which builds `dist/`, serves it on a loopback
 * port, and hands this the URL in `KRUDD_BASE_URL`.
 */

import { readFileSync, writeFileSync } from "node:fs";
import { CdpClient } from "./cdp";
import { type Browser, findChromium, launch } from "./chromium";
import { buildCompareExpression, type CompareStats, judge } from "./compare";
import { INIT_SCRIPT } from "./inject";
import { ensureDisplay } from "./xvfb";

/** Where the checked-in reference lives, relative to the workspace root — xtask always runs Node from there. */
const REFERENCE_PATH = "packages/shell/web/harness/reference.png";

/**
 * The emulated viewport.
 *
 * Fixed by `Emulation.setDeviceMetricsOverride` rather than inherited from
 * whatever window Chromium happens to open: the host window manager (or lack
 * of one, under Xvfb), the `--window-size` flag, and Chromium's own chrome
 * around the content area are none of them something this harness controls
 * precisely, and the reference image needs the capture to be the same size
 * on every machine that runs it.
 */
const VIEWPORT = { width: 400, height: 300 };

/**
 * Hides `#status` (`packages/shell/web/index.html`) before the screenshot.
 *
 * Without this, a canvas that renders nothing would still produce a
 * non-uniform screenshot — the status line's own text is always there,
 * anti-aliased, regardless of whether the WebGL2 backend drew a single
 * pixel. That would blunt exactly the check #827 asks for: a broken
 * renderer must not get to hide behind debug text that was never in
 * question. Removing the element rather than clipping a fixed pixel band out
 * of the capture: the line wraps to a second row whenever
 * `rendererDescription` is long (it includes the GPU/driver string, whose
 * length is not this harness's to predict), so a fixed clip band would be
 * guessing at a height that can silently change under it.
 */
const HIDE_STATUS_EXPRESSION = `
	(() => {
		for (const id of ["status", "modes"]) {
			const element = document.getElementById(id);
			if (element !== null) {
				element.style.display = "none";
			}
		}
	})();
`;

/**
 * Swipes to the board and back, twice, and reports whether the canvas came
 * through it.
 *
 * The one thing about the mode shell that a unit test cannot prove:
 * `boot()` takes the canvas for the life of the engine, and a second boot
 * against the same canvas fails because the WebGL2 context is already taken.
 * A shell that unmounted the viewport would therefore work perfectly once and
 * kill the engine on the second swipe — and the failure looks like a blank
 * canvas, which is exactly the "rendered nothing" this harness exists to
 * refuse.
 *
 * So it compares element identity across the round trip and leaves the page
 * back in game mode. The screenshot that follows is the other half of the
 * proof: the canvas is not merely still there, it is still drawing. Driven
 * through the rail's own buttons rather than a page-side test hook, because a
 * hook is a second code path that can pass while the real one is broken.
 */
const MODE_ROUND_TRIP_EXPRESSION = `
	(() => {
		const before = document.getElementById("viewport");
		const press = (mode) => {
			const button = document.querySelector(
				'#modes button[data-mode="' + mode + '"]',
			);
			if (button === null) {
				throw new Error("the page has no " + mode + " button");
			}
			button.click();
		};
		for (let i = 0; i < 2; i++) {
			press("board");
			press("game");
		}
		const after = document.getElementById("viewport");
		return {
			same: before === after && after !== null,
			connected: after !== null && after.isConnected,
			mode: document.body.dataset.mode,
		};
	})();
`;

/** Simulated milliseconds per stepped frame — a plain 60Hz tick. */
const FRAME_DT_MS = 1000 / 60;

/**
 * How many simulated frames run before the screenshot.
 *
 * 90 frames at the tick above is 1.5 simulated seconds. The demo's eight
 * entities (`ENTITY_COUNT` in `packages/shell/web/src/index.ts`) spawn at a
 * radius of 0.4 world units, drift outward at 0.25 units/second, and only
 * recycle back past a radius of 3.2 — `(3.2 - 0.4) / 0.25 = 11.2` simulated
 * seconds away. 1.5s lands comfortably mid-flight: the triangles are spread
 * out and visually distinct, and nothing has been recycled, so the frame is
 * fully determined by the tick count and nothing else. Long enough to prove
 * the animation is actually running — a screenshot at frame 0 would pass
 * even if `tick` silently became a no-op — and short enough that a run stays
 * fast.
 */
const FRAME_COUNT = 90;

/** How long to wait for the demo to boot and queue its first frame. */
const BOOT_TIMEOUT_MS = 15_000;

interface EvaluateResult<T> {
	result: { value?: T };
	exceptionDetails?: { text: string; exception?: { description?: string } };
}

async function main(): Promise<void> {
	// Must run before anything else touches the display or spawns Chromium —
	// see xvfb.ts. When there is no DISPLAY this re-execs and never returns.
	ensureDisplay();

	const baseUrl = process.env.KRUDD_BASE_URL;
	if (baseUrl === undefined || baseUrl === "") {
		throw new Error(
			"KRUDD_BASE_URL is not set — run this through `cargo xtask render-test`, " +
				"which builds dist/, serves it, and sets this",
		);
	}

	const browser = await launch(findChromium());
	try {
		await run(browser, baseUrl);
	} finally {
		browser.close();
	}
}

async function run(browser: Browser, baseUrl: string): Promise<void> {
	const cdp = new CdpClient(browser.webSocketDebuggerUrl);
	await cdp.ready();

	const pageErrors: string[] = [];
	cdp.on("Runtime.consoleAPICalled", (raw) => {
		const params = raw as {
			type: string;
			args: Array<{ value?: unknown; description?: unknown }>;
		};
		if (params.type !== "error") {
			return;
		}
		const message = params.args
			.map((arg) => String(arg.value ?? arg.description ?? ""))
			.join(" ");
		pageErrors.push(`console.error: ${message}`);
	});
	cdp.on("Runtime.exceptionThrown", (raw) => {
		const params = raw as {
			exceptionDetails: { text: string; exception?: { description?: string } };
		};
		const detail =
			params.exceptionDetails.exception?.description ??
			params.exceptionDetails.text;
		pageErrors.push(`uncaught exception: ${detail}`);
	});

	await cdp.send("Page.enable");
	await cdp.send("Runtime.enable");
	await cdp.send("Emulation.setDeviceMetricsOverride", {
		width: VIEWPORT.width,
		height: VIEWPORT.height,
		deviceScaleFactor: 1,
		mobile: false,
	});
	// The mode shell slides the board pane in and out over 220ms of *real*
	// time, which the virtual clock below does not touch. Asking for reduced
	// motion turns the transition off at the source, so a mode switch has
	// finished by the time anything is captured — rather than adding a
	// test-only hook to the page, or waiting out a duration this harness
	// would then have to keep in step with the stylesheet.
	await cdp.send("Emulation.setEmulatedMedia", {
		features: [{ name: "prefers-reduced-motion", value: "reduce" }],
	});
	// Registered before the real navigation below, so the demo's very first
	// `performance.now()` call and its very first `requestAnimationFrame`
	// already see the virtual clock rather than a real one.
	await cdp.send("Page.addScriptToEvaluateOnNewDocument", {
		source: INIT_SCRIPT,
	});
	await cdp.send("Page.navigate", { url: baseUrl });

	await waitForBoot(cdp, pageErrors);

	// Before the frames are stepped, so that everything after this point is
	// running on a canvas that has already survived four mode switches.
	const trip = await evaluate<{
		same: boolean;
		connected: boolean;
		mode: string;
	}>(cdp, MODE_ROUND_TRIP_EXPRESSION, true);
	if (trip.same !== true || trip.connected !== true || trip.mode !== "game") {
		fail([
			"the canvas did not survive swiping between modes:",
			`  same element: ${trip.same}`,
			`  still in the page: ${trip.connected}`,
			`  mode after the round trip: ${trip.mode}`,
			"the WebGL2 context is taken for the life of the engine — a shell that",
			"unmounts the viewport works once and kills the engine on the next swipe",
		]);
		return;
	}

	await evaluate(
		cdp,
		`for (let i = 0; i < ${FRAME_COUNT}; i++) { window.__kruddHarness.step(${FRAME_DT_MS}); }`,
		false,
	);
	await evaluate(cdp, "window.__kruddHarness.waitForPaint()", true);

	if (pageErrors.length > 0) {
		fail(["the page reported errors during the run:", ...pageErrors]);
		return;
	}

	// After waitForPaint, not before: the frame being screenshotted must be
	// the one `FRAME_COUNT` steps actually produced, with the status text
	// removed only for the capture itself rather than for the run.
	await evaluate(cdp, HIDE_STATUS_EXPRESSION, false);
	const screenshot = await cdp.send<{ data: string }>(
		"Page.captureScreenshot",
		{
			format: "png",
		},
	);

	// A maintenance escape hatch, not a CLI flag: regenerating the reference
	// is rare enough (only when the demo's own visuals intentionally change)
	// that it does not need a place in `xtask`'s shared `Options`, which every
	// other subcommand also parses.
	if (process.env.KRUDD_UPDATE_REFERENCE === "1") {
		writeFileSync(REFERENCE_PATH, Buffer.from(screenshot.data, "base64"));
		console.log(`render-test: wrote ${REFERENCE_PATH}`);
		return;
	}

	const referenceBase64 = readFileSync(REFERENCE_PATH).toString("base64");
	const expression = buildCompareExpression(screenshot.data, referenceBase64);
	const stats = await evaluate<CompareStats>(cdp, expression, true);

	const problems = judge(stats, REFERENCE_PATH);
	if (problems.length > 0) {
		fail(problems);
		return;
	}

	console.log(
		`render-test: PASS — ${stats.width}x${stats.height}, ` +
			`${stats.diffPixels}/${stats.totalPixels} pixels differ ` +
			`(${(stats.diffFraction * 100).toFixed(2)}%), within budget`,
	);
}

/**
 * Polls for the demo to have booted and queued its first (virtual) frame,
 * failing fast on a page error rather than only on the timeout — a boot
 * failure means `requestAnimationFrame` is never called at all, so without
 * this the run would report a generic timeout instead of the actual reason.
 */
async function waitForBoot(
	cdp: CdpClient,
	pageErrors: string[],
): Promise<void> {
	const deadline = Date.now() + BOOT_TIMEOUT_MS;
	while (Date.now() < deadline) {
		if (pageErrors.length > 0) {
			fail(["the page failed before it ever queued a frame:", ...pageErrors]);
			return;
		}
		const ready = await evaluate<boolean>(
			cdp,
			"typeof window.__kruddHarness !== 'undefined' && window.__kruddHarness.pending()",
			false,
		);
		if (ready === true) {
			return;
		}
		await sleep(100);
	}
	fail(["timed out waiting for the demo to boot and queue its first frame"]);
}

/** `Runtime.evaluate`, unwrapped and with the page's own exceptions surfaced. */
async function evaluate<T>(
	cdp: CdpClient,
	expression: string,
	awaitPromise: boolean,
): Promise<T> {
	const response = await cdp.send<EvaluateResult<T>>("Runtime.evaluate", {
		expression,
		awaitPromise,
		returnByValue: true,
	});
	if (response.exceptionDetails !== undefined) {
		const detail =
			response.exceptionDetails.exception?.description ??
			response.exceptionDetails.text;
		throw new Error(`page evaluation failed: ${detail}`);
	}
	return response.result.value as T;
}

function sleep(ms: number): Promise<void> {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

/** Prints every reason the run failed and exits non-zero. */
function fail(reasons: string[]): void {
	console.error("render-test: FAIL");
	for (const reason of reasons) {
		console.error(`  ${reason}`);
	}
	process.exitCode = 1;
}

await main();
