// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The rules for moving between the two modes.
 *
 * These are the ones that cannot be checked by looking at the page. Whether a
 * drag navigates or pans is decided by arithmetic over where it began, and
 * the failure — panning a board and being thrown into the game — is a
 * gesture-scale bug that a screenshot cannot see and a person can only find
 * by getting annoyed on a phone. So the arithmetic lives apart from the DOM
 * and is asserted here.
 *
 * The other half of the rule, that the canvas survives any number of
 * switches, is not something a unit test can prove: it needs a real WebGL2
 * context and a real page. `cargo xtask render-test` does that end to end —
 * it swipes across and back four times before it takes its screenshot.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import {
	EDGE_WIDTH,
	grabsEdge,
	isMode,
	keyDirection,
	MODES,
	neighbour,
	SWIPE_DISTANCE,
	swipeTarget,
} from "../src/track";

/** A phone, in CSS pixels. The case the whole design is aimed at. */
const WIDTH = 390;

test("the track is the game and the board, in that order", () => {
	assert.deepEqual([...MODES], ["game", "board"]);
	assert.ok(isMode("board"));
	assert.ok(!isMode("pause"), "two modes, and that is the whole list for now");
});

test("the track does not wrap", () => {
	// Swiping the same way twice returning you to where you started reads as
	// the gesture having failed, rather than as having run out of track.
	assert.equal(neighbour("game", 1), "board");
	assert.equal(neighbour("board", -1), "game");
	assert.equal(neighbour("game", -1), undefined);
	assert.equal(neighbour("board", 1), undefined);
});

test("a drag from the interior is content, not navigation", () => {
	// The rule the board depends on. Panning a board must never throw you
	// into the game.
	assert.ok(!grabsEdge(WIDTH / 2, WIDTH), "the middle pans");
	assert.ok(!grabsEdge(EDGE_WIDTH + 1, WIDTH));
	assert.ok(!grabsEdge(WIDTH - EDGE_WIDTH - 1, WIDTH));
});

test("a drag from either edge navigates", () => {
	assert.ok(grabsEdge(0, WIDTH));
	assert.ok(grabsEdge(EDGE_WIDTH, WIDTH));
	assert.ok(grabsEdge(WIDTH, WIDTH));
	assert.ok(grabsEdge(WIDTH - EDGE_WIDTH, WIDTH));
});

test("a swipe has to travel before it changes anything", () => {
	// A thumb resting on the edge of a phone is a drag of a pixel or two.
	assert.equal(swipeTarget("game", -1), undefined);
	assert.equal(swipeTarget("game", -(SWIPE_DISTANCE - 1)), undefined);
	assert.equal(swipeTarget("game", -SWIPE_DISTANCE), "board");
});

test("dragging left reveals what is to the right", () => {
	assert.equal(
		swipeTarget("game", -200),
		"board",
		"the game is left of the board",
	);
	assert.equal(swipeTarget("board", 200), "game");
	assert.equal(
		swipeTarget("game", 200),
		undefined,
		"there is no track that way",
	);
	assert.equal(swipeTarget("board", -200), undefined);
});

test("the arrows walk the track and nothing else does", () => {
	assert.equal(keyDirection("ArrowRight"), 1);
	assert.equal(keyDirection("ArrowLeft"), -1);
	assert.equal(keyDirection("ArrowUp"), undefined);
	assert.equal(keyDirection("Tab"), undefined);
	assert.equal(keyDirection("a"), undefined);
});
