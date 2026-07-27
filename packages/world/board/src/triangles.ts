// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The triangles project: the eight drifting triangles the engine runs today,
 * written as a board.
 *
 * This is the first project, and the thing the initiative's proof of life is
 * performed on — open the board, disconnect `paint → draw entities`, swipe to
 * the game, and the triangles are gone. So the numbers in it are not
 * illustrative: `count`, `radius`, `speed`, `limit` and `scale` are the
 * constants `packages/shell/web/src/index.ts` and `crates/shell/web/src/lib.rs`
 * run on, and the interpreter reproducing the page pixel for pixel depends on
 * them staying that way.
 *
 * Most of them are the kind's default rather than written out again here. A
 * document that restated every default would be a document where changing a
 * default changed nothing, and the diff on a saved project would be noise
 * around the one value somebody actually moved.
 *
 * A TypeScript module rather than a `.json` file, for now: as a module the
 * fixture is typechecked, so a fixture that drifts from the schema fails the
 * build rather than only failing a test. What a project looks like *on disk*
 * is a separate question, and the round-trip test is what proves this
 * survives the trip.
 */

import type { Project } from "./document";
import { DOCUMENT_VERSION } from "./document";
import { FRAME, FRAME_BOARD } from "./frame";

/** The board the project opens on. */
export const ROOT_BOARD = "triangles";

/**
 * The wire the proof of life cuts.
 *
 * Named because more than one thing refers to it — the end-to-end test, and
 * anything that wants to demonstrate the board driving the game.
 */
export const PAINT_TO_DRAW = "exec-paint-draw";

/** The triangles project. */
export const TRIANGLES: Project = {
	version: DOCUMENT_VERSION,
	root: ROOT_BOARD,
	boards: {
		[ROOT_BOARD]: {
			title: "triangles",
			// Declared explicitly rather than left implicit in `spawn-ring`'s
			// output ports: a column is shared state with a lifetime longer than
			// any one wire, and cutting the wire out of `spawn-ring` must not
			// take `position` or `velocity` down with it.
			columns: [
				{ name: "position", kind: "vec3" },
				{ name: "velocity", kind: "vec3" },
			],
			nodes: [
				{ id: "start", kind: "start", lane: "start", column: 0 },
				{ id: "ring", kind: "spawn-ring", lane: "start", column: 1 },

				{ id: "step", kind: "step", lane: "step", column: 0 },
				{ id: "integrate", kind: "integrate", lane: "step", column: 1 },
				{ id: "recycle", kind: "recycle", lane: "step", column: 2 },

				{ id: "paint", kind: "paint", lane: "paint", column: 0 },
				{
					id: "draw",
					kind: "draw-entities",
					lane: "paint",
					column: 1,
					// Opening this node is opening the frame itself.
					board: FRAME_BOARD,
				},
			],
			wires: [
				{
					id: "exec-start-ring",
					from: { node: "start", port: "out" },
					to: { node: "ring", port: "in" },
					kind: "exec",
				},
				{
					id: "exec-step-integrate",
					from: { node: "step", port: "out" },
					to: { node: "integrate", port: "in" },
					kind: "exec",
				},
				{
					id: "exec-integrate-recycle",
					from: { node: "integrate", port: "out" },
					to: { node: "recycle", port: "in" },
					kind: "exec",
				},
				{
					id: PAINT_TO_DRAW,
					from: { node: "paint", port: "out" },
					to: { node: "draw", port: "in" },
					kind: "exec",
				},
				{
					id: "data-ring-integrate",
					from: { node: "ring", port: "position" },
					to: { node: "integrate", port: "position" },
					kind: "data",
				},
				{
					id: "data-ring-velocity",
					from: { node: "ring", port: "velocity" },
					to: { node: "integrate", port: "velocity" },
					kind: "data",
				},
				{
					id: "data-integrate-recycle",
					from: { node: "integrate", port: "position" },
					to: { node: "recycle", port: "position" },
					kind: "data",
				},
				{
					id: "data-recycle-draw",
					from: { node: "recycle", port: "position" },
					to: { node: "draw", port: "position" },
					kind: "data",
				},
			],
		},

		// The frame the engine builds, held by every project that draws — see
		// `./frame`, which is where it lives now that there is more than one
		// project to open into it.
		[FRAME_BOARD]: FRAME,
	},
};
