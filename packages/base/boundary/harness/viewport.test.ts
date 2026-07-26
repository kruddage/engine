// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The drawing-buffer contract, asserted: the page never asks the renderer for
 * a surface it cannot configure.
 *
 * A canvas at `devicePixelRatio` on a portrait phone is routinely taller than
 * the 2048 the WebGL2 backend holds a surface to, and wgpu answers that with
 * a validation error rather than a clamp — "`Surface` width and height must
 * be within the maximum supported texture size" — which took the whole page
 * down on Android. `fitCanvas` scales the buffer to fit, and it delegates the
 * arithmetic to the wasm module so that the page and the renderer cannot
 * disagree about the result.
 *
 * That delegation is the reason these run here rather than in Rust: the sizes
 * are decided in Rust and asserted there too, but "the canvas the page hands
 * the renderer is one the renderer accepts" is a claim about the two halves
 * together, and only the real module can settle it.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs. See
 * `docs/boundary.md`.
 */

import assert from "node:assert/strict";
import test from "node:test";
import { fitCanvas } from "../src/index";
import { load } from "./load";

/**
 * The WebGL2 floor the backend requests, and so the largest either dimension
 * of the drawing buffer may be — `MAX_SURFACE_EXTENT` in `krudd-webgl`.
 *
 * Written out rather than imported: this is the number the *other* side is
 * supposed to be enforcing, and a test that reads its expectation out of the
 * thing under test would pass whatever that thing did.
 */
const MAX_EXTENT = 2048;

/**
 * A canvas, as much of one as `fitCanvas` touches: a layout size it reads and
 * a drawing buffer it writes. Node has no DOM, and a real canvas would drag
 * in a browser to test three property assignments.
 */
function stubCanvas(clientWidth: number, clientHeight: number) {
	return {
		clientWidth,
		clientHeight,
		width: 0,
		height: 0,
	} as unknown as HTMLCanvasElement;
}

test("a canvas within the limit gets exactly the buffer its layout asks for", async () => {
	await load();
	const canvas = stubCanvas(800, 600);

	assert.equal(fitCanvas(canvas, 2), true, "0x0 to 1600x1200 is a change");
	assert.equal(canvas.width, 1600);
	assert.equal(canvas.height, 1200);
});

test("a portrait phone's buffer is fitted rather than refused", async () => {
	await load();
	// 360x752 CSS pixels at a device pixel ratio of 3 — the 1080x2256 the
	// error reported.
	const canvas = stubCanvas(360, 752);
	fitCanvas(canvas, 3);

	assert.equal(canvas.height, MAX_EXTENT, "the longer side lands on the cap");
	assert.ok(canvas.width <= MAX_EXTENT);
	assert.ok(
		Math.abs(canvas.width / canvas.height - 1080 / 2256) < 0.001,
		"both sides scale together — an independently clamped one is stretched " +
			"back over the CSS box and reads as a squashed image",
	);
});

test("a fitted canvas is left alone by the next resize", async () => {
	await load();
	const canvas = stubCanvas(360, 752);
	assert.equal(fitCanvas(canvas, 3), true);

	assert.equal(
		fitCanvas(canvas, 3),
		false,
		"the page must settle on the size the renderer settles on, or every " +
			"resize event reconfigures the swapchain for a size it will refuse",
	);
});

test("the engine renders at the size the canvas was fitted to", async () => {
	const canvas = stubCanvas(360, 752);
	const { world } = await load();
	fitCanvas(canvas, 3);
	world.resize(canvas.width, canvas.height);

	assert.deepEqual(
		world.viewport,
		{ width: canvas.width, height: canvas.height },
		"the engine took the buffer as given — anything else means the frame " +
			"and the surface describe different sizes",
	);
});

test("an unlaid-out canvas still gets a legal buffer", async () => {
	await load();
	// display: none, or a boot that ran before first layout.
	const canvas = stubCanvas(0, 0);
	fitCanvas(canvas, 3);

	assert.equal(canvas.width, 1);
	assert.equal(canvas.height, 1);
});
