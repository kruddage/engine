// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The frame, one level down: what `Draw Entities` opens into.
 *
 * Its own module rather than part of the triangles project, because it is not
 * part of any one project — it describes the frame *the engine* builds, so
 * every project that draws anything opens into the same four nodes. Held here,
 * a second project gets the frame by holding this board rather than by
 * restating it, and a change to what a frame is reaches both.
 *
 * Viewable rather than runnable: none of these kinds has a `run`, and a board
 * that ran them would be a second implementation of the frame beside the real
 * one in `crates/shell/web`. Opening `Draw Entities` shows clear → camera →
 * draw → present because that is what the engine already does, which is why a
 * node carrying this board still executes its own `run` — see `run.ts`.
 */

import type { Board } from "./document";

/** The board id a `draw-entities` node opens into. */
export const FRAME_BOARD = "frame";

/** The frame itself, as a board. */
export const FRAME: Board = {
	title: "Draw Entities",
	// One level down, the three lanes are stages of a frame rather than of the
	// game. Saying "every frame, to the screen" in here would be telling
	// somebody standing inside a frame about frames.
	lanes: {
		start: "set up the frame",
		step: "draw into it",
		paint: "hand it to the screen",
	},
	// The frame is the detail, and Simple is the level where the detail is not
	// shown. Opening Draw Entities at Simple opens nothing.
	pro: true,
	nodes: [
		{ id: "begin", kind: "begin-frame", lane: "start", column: 0 },
		{ id: "clear", kind: "clear", lane: "start", column: 1 },

		{ id: "camera", kind: "camera", lane: "step", column: 0 },
		{ id: "draw", kind: "draw", lane: "step", column: 1 },

		{ id: "present", kind: "present", lane: "paint", column: 0 },
	],
	wires: [
		{
			id: "exec-begin-clear",
			from: { node: "begin", port: "out" },
			to: { node: "clear", port: "in" },
			kind: "exec",
		},
		{
			id: "data-begin-clear",
			from: { node: "begin", port: "frame" },
			to: { node: "clear", port: "frame" },
			kind: "data",
		},
		{
			id: "exec-camera-draw",
			from: { node: "camera", port: "out" },
			to: { node: "draw", port: "in" },
			kind: "exec",
		},
		{
			id: "data-camera-draw",
			from: { node: "camera", port: "viewProjection" },
			to: { node: "draw", port: "viewProjection" },
			kind: "data",
		},
	],
};
