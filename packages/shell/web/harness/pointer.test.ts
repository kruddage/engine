// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The pointer source, as arithmetic.
 *
 * Everything here is `PointerTrack` fed plain numbers — no DOM, no canvas, no
 * `PointerEvent` — for the same reason `track.test.ts` asserts `track.ts`
 * this way: the rule that matters is decided by arithmetic over a position
 * and a transition, and asserting it here means it is proven on every run
 * rather than only by tapping a phone.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import { PointerTrack } from "../src/pointer";
import { EDGE_WIDTH } from "../src/track";

/** A phone, portrait: the non-square case the design is aimed at. */
const PHONE_WIDTH = 390;
const PHONE_HEIGHT = 844;

test("a press held across several frames reads as pressed exactly once", () => {
	// The single most important behaviour in this file: the pre-rewrite engine
	// debounced on `g_last_sel` for exactly this reason, and without the edge a
	// held press fills a board in one frame.
	const track = new PointerTrack();
	track.down(100, 100, PHONE_WIDTH, PHONE_HEIGHT);

	assert.equal(track.sample().pressed, 1, "the frame the press began on");
	assert.equal(track.sample().pressed, 0, "still held, one frame later");
	assert.equal(track.sample().pressed, 0, "still held, another frame on");
	assert.equal(track.sample().pressed, 0, "held for as long as it likes");
});

test("release and press again reads as pressed a second time", () => {
	const track = new PointerTrack();
	track.down(100, 100, PHONE_WIDTH, PHONE_HEIGHT);
	assert.equal(track.sample().pressed, 1);
	assert.equal(track.sample().pressed, 0, "held");

	track.up();
	assert.equal(track.sample().pressed, 0, "released, nothing new to report");

	track.down(120, 140, PHONE_WIDTH, PHONE_HEIGHT);
	assert.equal(track.sample().pressed, 1, "a second press, a second edge");
	assert.equal(track.sample().pressed, 0, "and it debounces again");
});

test("coordinates are stable across a viewport resize", () => {
	// The same relative position — a quarter of the way across, a quarter of
	// the way down — read at two different viewport sizes must come back the
	// same normalised value. A convention that baked in pixels captured under
	// the old size would move the tap the moment the window changed shape.
	const narrow = new PointerTrack();
	narrow.down(100, 50, 400, 200);
	const before = narrow.sample();

	const wide = new PointerTrack();
	wide.down(200, 100, 800, 400);
	const after = wide.sample();

	assert.equal(
		before.x,
		after.x,
		"the fraction across is unchanged by a resize",
	);
	assert.equal(before.y, after.y, "the fraction down is unchanged by a resize");
	assert.equal(before.x, 0.25);
	assert.equal(before.y, 0.25);
});

test("coordinates are correct at a non-square viewport", () => {
	// The phone-shaped case #853 describes: width and height are divided
	// independently, so neither axis borrows the other's scale. Every `x`
	// here sits clear of `grabsEdge`'s zone — the left and right 32px bands
	// are the mode shell's the full height of the screen, so `x = 0` or
	// `x = width` would test the swipe boundary rather than this arithmetic;
	// that boundary has its own tests below.
	const track = new PointerTrack();

	// Top edge, dead centre horizontally.
	track.down(PHONE_WIDTH / 2, 0, PHONE_WIDTH, PHONE_HEIGHT);
	assert.deepEqual(
		{ x: track.sample().x, y: track.sample().y },
		{ x: 0.5, y: 0 },
		"the top edge, mid-width",
	);
	track.up();

	// Bottom edge, dead centre horizontally.
	track.down(PHONE_WIDTH / 2, PHONE_HEIGHT, PHONE_WIDTH, PHONE_HEIGHT);
	const bottom = track.sample();
	assert.equal(bottom.x, 0.5);
	assert.equal(bottom.y, 1, "the bottom edge");
	track.up();

	// Off-centre on both axes: dividing by the wrong extent would show up
	// here as a value the other axis's fraction does not match.
	track.down(300, 100, PHONE_WIDTH, PHONE_HEIGHT);
	const point = track.sample();
	assert.equal(point.x, 300 / PHONE_WIDTH);
	assert.equal(point.y, 100 / PHONE_HEIGHT);
});

test("a pointer event in the edge-swipe zone does not reach the graph", () => {
	// `track.ts` already draws this line for navigation; the graph must never
	// see a press that landed inside it, or a swipe meant to navigate would
	// also place a mark.
	const track = new PointerTrack();
	track.down(EDGE_WIDTH - 1, 400, PHONE_WIDTH, PHONE_HEIGHT);

	const frame = track.sample();
	assert.equal(frame.pressed, 0, "an edge press never reads as pressed");
	assert.equal(frame.x, 0, "and it never moves the coordinate either");
	assert.equal(frame.y, 0);
});

test("a pointer event in the interior does reach it", () => {
	const track = new PointerTrack();
	const interior = PHONE_WIDTH / 2;
	track.down(interior, 400, PHONE_WIDTH, PHONE_HEIGHT);

	const frame = track.sample();
	assert.equal(frame.pressed, 1);
	assert.equal(frame.x, interior / PHONE_WIDTH);
	assert.equal(frame.y, 400 / PHONE_HEIGHT);
});

test("a move before any press is ignored", () => {
	const track = new PointerTrack();
	track.move(200, 400, PHONE_WIDTH, PHONE_HEIGHT);
	const frame = track.sample();
	assert.equal(frame.pressed, 0);
	assert.equal(
		frame.x,
		0,
		"a move with nothing down does not relocate the pointer",
	);
});

test("a move while held updates the coordinate but never presses again", () => {
	const track = new PointerTrack();
	// x = 100 is clear of the edge-swipe zone; the drag that starts at the
	// edge and moves inward is its own test, below.
	track.down(100, 10, PHONE_WIDTH, PHONE_HEIGHT);
	track.move(200, 400, PHONE_WIDTH, PHONE_HEIGHT);

	const frame = track.sample();
	assert.equal(frame.pressed, 1, "the edge from the down, not from the move");
	assert.equal(
		frame.x,
		200 / PHONE_WIDTH,
		"but the coordinate tracks the move",
	);
	assert.equal(frame.y, 400 / PHONE_HEIGHT);
});

test("a drag that began at the edge stays ignored however far it moves", () => {
	// Decided once, at `down`, and never re-decided — the same rule
	// `track.ts` applies to its own gesture. A drag from the edge into the
	// interior must not suddenly start reaching the graph.
	const track = new PointerTrack();
	track.down(0, 100, PHONE_WIDTH, PHONE_HEIGHT);
	track.move(PHONE_WIDTH / 2, 100, PHONE_WIDTH, PHONE_HEIGHT);

	const frame = track.sample();
	assert.equal(frame.pressed, 0);
	assert.equal(frame.x, 0, "the move never took effect either");
});

test("releasing after an edge gesture leaves the next press free to register", () => {
	const track = new PointerTrack();
	track.down(0, 100, PHONE_WIDTH, PHONE_HEIGHT);
	track.up();

	track.down(PHONE_WIDTH / 2, 100, PHONE_WIDTH, PHONE_HEIGHT);
	assert.equal(
		track.sample().pressed,
		1,
		"a fresh press in the interior registers",
	);
});
