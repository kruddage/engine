// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Descending into a board, and finding the way back.
 *
 * A breadcrumb with an off-by-one in it drops a level or refuses to come home,
 * and neither shows up in a screenshot of a board that happens to be the right
 * one. So the path is a rule apart from the DOM, and these are the rules.
 *
 * The other half is that Simple and Pro are one board at two detail levels
 * rather than two editors — which means the level has to be able to say "not
 * that board", and has to be able to climb you out of one you are already
 * standing in.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import { FRAME_BOARD, ROOT_BOARD, TRIANGLES } from "@krudd/board";
import { LANE_LABELS, Trail } from "../src/index";

test("a project opens on its root board", () => {
	const trail = new Trail(TRIANGLES);
	assert.equal(trail.here, ROOT_BOARD);
	assert.equal(trail.depth, 0);
	assert.deepEqual(
		trail.crumbs.map((crumb) => crumb.title),
		["triangles"],
	);
});

test("opening a node's board descends, and the breadcrumb walks back up", () => {
	const trail = new Trail(TRIANGLES);
	assert.ok(trail.enter(FRAME_BOARD, "pro"));

	assert.equal(trail.here, FRAME_BOARD);
	assert.equal(trail.depth, 1);
	assert.deepEqual(
		trail.crumbs.map((crumb) => crumb.title),
		["triangles", "Draw Entities"],
		"the whole way down, so every level is a way back",
	);

	assert.ok(trail.up());
	assert.equal(trail.here, ROOT_BOARD);
	assert.ok(!trail.up(), "the root is the floor");
	assert.equal(trail.here, ROOT_BOARD);
});

test("tapping a crumb goes back to it, and tapping where you are does nothing", () => {
	const trail = new Trail(TRIANGLES);
	trail.enter(FRAME_BOARD, "pro");

	assert.ok(!trail.goTo(FRAME_BOARD), "you are already standing here");
	assert.ok(trail.goTo(ROOT_BOARD));
	assert.equal(trail.here, ROOT_BOARD);
	assert.equal(trail.depth, 0);
	assert.ok(
		!trail.goTo(FRAME_BOARD),
		"a board that is not on the path is not a crumb, so it is not a way back",
	);
});

test("a board cannot be descended into twice, or at all if it is not there", () => {
	const trail = new Trail(TRIANGLES);
	assert.ok(!trail.enter("nonesuch", "pro"), "the project does not hold it");
	assert.ok(trail.enter(FRAME_BOARD, "pro"));
	assert.ok(
		!trail.enter(ROOT_BOARD, "pro"),
		"descending into a board already on the path is a breadcrumb with no top",
	);
});

test("the frame does not open at simple, because the frame is the detail", () => {
	// An eight-year-old placing a Draw Entities node has no business being
	// dropped into clear → camera → draw → present.
	const trail = new Trail(TRIANGLES);
	assert.ok(!trail.enter(FRAME_BOARD, "simple"));
	assert.equal(trail.here, ROOT_BOARD);
	assert.ok(trail.enter(FRAME_BOARD, "pro"), "and does at pro");
});

test("switching to simple climbs out of a board simple cannot reach", () => {
	// Otherwise you are left looking at something the level you just chose says
	// does not exist.
	const trail = new Trail(TRIANGLES);
	trail.enter(FRAME_BOARD, "pro");

	assert.ok(trail.settle("simple"), "it had to move");
	assert.equal(trail.here, ROOT_BOARD);
	assert.ok(!trail.settle("simple"), "and stays put once it has");
	assert.ok(!trail.settle("pro"), "pro reaches everywhere, so it never moves");
});

test("a lane label reads correctly for the board you are standing in", () => {
	// "every frame, to the screen" is about the game. One level down, inside a
	// frame, the third lane is about the frame — saying the first would be
	// telling somebody standing inside a frame about frames.
	const trail = new Trail(TRIANGLES);
	assert.deepEqual(trail.labels, LANE_LABELS);
	assert.match(trail.labels.paint, /to the screen/);

	trail.enter(FRAME_BOARD, "pro");
	assert.notDeepEqual(trail.labels, LANE_LABELS);
	assert.equal(trail.labels.start, "set up the frame");
	assert.equal(trail.labels.step, "draw into it");
	assert.equal(trail.labels.paint, "hand it to the screen");
});

test("the frame board reads clear → camera → draw → present", () => {
	// #839's "the frame graph is viewable", from the fixture document and with
	// no pass DAG underneath it.
	const trail = new Trail(TRIANGLES);
	trail.enter(FRAME_BOARD, "pro");
	assert.deepEqual(
		trail.board?.nodes.map((node) => node.id),
		["begin", "clear", "camera", "draw", "present"],
	);
});
