// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Reading a `color` param, and what an unreadable one does.
 *
 * The tolerance is the part worth pinning down: `validate` only checks that a
 * `color` param is text, the settings sheet edits a live document, and half of
 * `#e2574c` is text too. So the interesting cases here are the ones that are
 * not colours at all — see `../src/colour`'s own docs for why they read as
 * white rather than throwing.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import { rgbOf, WHITE } from "../src/index";

test("a six-digit hex is three channels in [0, 1], in order", () => {
	assert.deepEqual(rgbOf("#000000"), [0, 0, 0]);
	assert.deepEqual(rgbOf("#ffffff"), [1, 1, 1]);
	assert.deepEqual(rgbOf("#ff0000"), [1, 0, 0]);
	assert.deepEqual(rgbOf("#0000ff"), [0, 0, 1]);
});

test("channels are taken as written, not converted from sRGB", () => {
	// 0x80 / 255, not the 0.216 an sRGB-to-linear conversion would give. The
	// vertex colours this multiplies into are written the same way.
	const [r] = rgbOf("#808080");
	assert.ok(Math.abs(r - 128 / 255) < 1e-9);
});

test("the three-digit shorthand doubles each digit", () => {
	assert.deepEqual(rgbOf("#f00"), rgbOf("#ff0000"));
	assert.deepEqual(rgbOf("#abc"), rgbOf("#aabbcc"));
});

test("case and surrounding space do not matter", () => {
	assert.deepEqual(rgbOf("#E2574C"), rgbOf("#e2574c"));
	assert.deepEqual(rgbOf("  #e2574c "), rgbOf("#e2574c"));
});

test("anything that is not a hex colour reads as white, and does not throw", () => {
	for (const value of [
		"",
		"red",
		"#",
		// Halfway through typing `#e2574c` — the case the tolerance exists for.
		"#e2574",
		"#gggggg",
		"#12345678",
		"rgb(1, 2, 3)",
	]) {
		assert.deepEqual(rgbOf(value), WHITE, `\`${value}\` should read as white`);
	}
});
