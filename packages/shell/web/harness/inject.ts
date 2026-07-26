// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The script injected into the page before any of its own code runs, via
 * `Page.addScriptToEvaluateOnNewDocument`.
 *
 * It solves determinism, not blankness — the headless/blank trap is handled
 * by never running headless at all (`xvfb.ts`). What is left is the demo's
 * own animation: eight triangles drifting outward under a real
 * `performance.now()`-driven `requestAnimationFrame`, which gives a
 * different frame every run on a real clock. So this replaces both with a
 * virtual clock the driver steps by hand: `performance.now` returns a value
 * that only moves when told to, and `requestAnimationFrame` queues the
 * callback instead of scheduling it against the display.
 * `window.__kruddHarness.step(dtMs)` advances the clock and runs exactly the
 * one callback queued for it — one simulated frame, the length the driver
 * chose, not the display's.
 *
 * This is a plain string rather than a checked TypeScript module on purpose:
 * it runs inside the page, under whatever globals *that* environment
 * defines, not under Node — the two do not share a type system, and this
 * package's `tsconfig.harness.json` already has both `dom` and `@types/node`
 * in scope for the files that really do run under Node (`cdp.ts` works
 * around exactly that overlap). Writing browser-side source as a string
 * asset is not a new idea in this repository either —
 * `crates/render/webgl/src/triangle.wgsl` is source for a different runtime,
 * kept as a string/file rather than forced through this package's own
 * checker.
 *
 * The other half of what this injects is `webglcontextlost`: a lost context
 * does not throw and does not reach `console.error` on its own, so it is
 * exactly the kind of silent failure #827 exists to stop.
 * `document.addEventListener` with `capture: true` sees it regardless of
 * whether the event bubbles, because the capture phase runs on the way
 * *down* to the target before bubbling is even decided — so this does not
 * need to know the canvas exists yet, only that the document does.
 */
export const INIT_SCRIPT = `
(() => {
	let virtualNow = 0;
	let queuedFrame = null;
	const nativeRequestAnimationFrame = window.requestAnimationFrame.bind(window);

	window.performance.now = () => virtualNow;
	window.requestAnimationFrame = (callback) => {
		queuedFrame = callback;
		return 1;
	};

	window.__kruddHarness = {
		pending: () => queuedFrame !== null,
		step: (dtMs) => {
			virtualNow += dtMs;
			const callback = queuedFrame;
			queuedFrame = null;
			if (callback !== null) {
				callback(virtualNow);
			}
		},
		// Two *real* animation frames, so whatever the last step() drew has
		// actually reached the compositor before render-test.ts takes its
		// screenshot. A single frame is sometimes not enough for a change to
		// have been picked up; two is the standard belt-and-braces wait for
		// "has this paint actually happened yet".
		waitForPaint: () =>
			new Promise((resolve) => {
				nativeRequestAnimationFrame(() => {
					nativeRequestAnimationFrame(() => resolve(undefined));
				});
			}),
	};

	document.addEventListener(
		"webglcontextlost",
		(event) => {
			console.error("krudd render-test: WebGL context lost", event);
		},
		true,
	);
})();
`;
