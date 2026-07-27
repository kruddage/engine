// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The tic-tac-toe project: nine cells, two players, written as a board.
 *
 * The second project, and the one that answers #866's question — whether the
 * board can hold a game the triangles demo never asked it to hold. Nothing
 * below this file knows what tic-tac-toe is: `spawn-grid` lays out a grid,
 * `place-mark` runs a turn-based placement over columns it is told the names
 * of, and `colour-marks` tints a column by another column. The game is which
 * of them are wired together and what their params say, which is what a
 * project is.
 *
 * ## What each lane is
 *
 * - **Start** — `spawn-grid` makes the nine cells, once, when the board opens.
 * - **Step** — `place-mark` takes this frame's press and runs the whole move:
 *   legality, turn alternation, the win table, the draw, the restart. See its
 *   entry in `./kinds` for why that is one node rather than three.
 * - **Paint** — `colour-marks` writes the `colour` column from `mark` and
 *   `won`, and `draw-entities` draws. The tint is paint-lane work because it
 *   is how the state *looks* rather than part of the move: cut
 *   [`PAINT_TO_COLOURS`] and the screen goes blank while the taps go on
 *   landing and the turns go on alternating underneath, which is this
 *   project's proof of life and exactly what the triangles' `PAINT_TO_DRAW`
 *   demonstrates one board over.
 *
 * ## The two `size` params are the same number twice
 *
 * `spawn-grid` lays the cells out over `size` world units and `place-mark`
 * quantises a press against `size`. They are separate params on separate
 * nodes — a node never reads another node's settings — so a project that sets
 * one has to set the other, or the cells drawn and the cells picked drift
 * apart. Both are 3 here rather than the kinds' default of 2, which is what
 * makes the grid fill the camera's box (`extent` 2, so world y runs -2 to 2)
 * rather than sitting in the middle quarter of it.
 *
 * A TypeScript module rather than a `.json` file, for the reason `triangles.ts`
 * gives: as a module the project is typechecked, so one that drifts from the
 * schema fails the build rather than only failing a test. It is a project file
 * in every other sense — `tictactoe.test.ts` holds it to the same
 * `serializeProject` → `parseProject` round trip a saved file takes, and the
 * page boots it through the same `Runner` any opened file gets.
 */

import type { Project } from "./document";
import { DOCUMENT_VERSION } from "./document";
import { FRAME, FRAME_BOARD } from "./frame";

/** The board the project opens on. */
export const TICTACTOE_BOARD = "tic-tac-toe";

/**
 * The grid's width and height in world units, on both the node that draws it
 * and the node that picks against it. See the module docs — one number, and
 * two nodes that each have to be told it.
 */
const GRID_SIZE = 3;

/**
 * The wire the proof of life cuts: the screen goes blank and the game plays on.
 *
 * Named for the same reason `PAINT_TO_DRAW` is — more than one thing refers to
 * it, and a test that cut "the first wire in the paint lane" would be a test
 * that broke the moment a node was inserted.
 */
export const PAINT_TO_COLOURS = "exec-paint-colours";

/** The tic-tac-toe project. */
export const TICTACTOE: Project = {
	version: DOCUMENT_VERSION,
	root: TICTACTOE_BOARD,
	boards: {
		[TICTACTOE_BOARD]: {
			title: "tic-tac-toe",
			// Six columns, all of them board-level: `mark` is written by
			// `place-mark` and read by `colour-marks`, so it outlives either of
			// them and cutting a wire must not take it down. `turn` and `over`
			// are scalars living at slot 0 of a full-width column — a wart
			// #866 took deliberately, in exchange for every node staying a pure
			// function of (columns, params).
			columns: [
				{ name: "position", kind: "vec3" },
				{ name: "colour", kind: "vec3" },
				{ name: "mark", kind: "u32" },
				{ name: "won", kind: "u32" },
				{ name: "turn", kind: "u32" },
				{ name: "over", kind: "u32" },
			],
			nodes: [
				{ id: "start", kind: "start", lane: "start", column: 0 },
				{
					id: "grid",
					kind: "spawn-grid",
					lane: "start",
					column: 1,
					params: { size: GRID_SIZE },
				},

				{ id: "step", kind: "step", lane: "step", column: 0 },
				{
					id: "place",
					kind: "place-mark",
					lane: "step",
					column: 1,
					params: { size: GRID_SIZE },
				},

				{ id: "paint", kind: "paint", lane: "paint", column: 0 },
				{ id: "colours", kind: "colour-marks", lane: "paint", column: 1 },
				{
					id: "draw",
					kind: "draw-entities",
					lane: "paint",
					column: 2,
					// Cells sit `size / 3` apart — one world unit at this grid
					// size — so 0.8 fills a cell while leaving it visibly its
					// own.
					params: { scale: 0.8 },
					// Opening this node is opening the frame itself.
					board: FRAME_BOARD,
				},
			],
			wires: [
				{
					id: "exec-start-grid",
					from: { node: "start", port: "out" },
					to: { node: "grid", port: "in" },
					kind: "exec",
				},
				{
					id: "exec-step-place",
					from: { node: "step", port: "out" },
					to: { node: "place", port: "in" },
					kind: "exec",
				},
				{
					id: PAINT_TO_COLOURS,
					from: { node: "paint", port: "out" },
					to: { node: "colours", port: "in" },
					kind: "exec",
				},
				{
					id: "exec-colours-draw",
					from: { node: "colours", port: "out" },
					to: { node: "draw", port: "in" },
					kind: "exec",
				},
				{
					id: "data-grid-draw",
					from: { node: "grid", port: "position" },
					to: { node: "draw", port: "position" },
					kind: "data",
				},
				{
					id: "data-place-colours-mark",
					from: { node: "place", port: "mark" },
					to: { node: "colours", port: "mark" },
					kind: "data",
				},
				{
					id: "data-place-colours-won",
					from: { node: "place", port: "won" },
					to: { node: "colours", port: "won" },
					kind: "data",
				},
				{
					id: "data-colours-draw",
					from: { node: "colours", port: "colour" },
					to: { node: "draw", port: "colour" },
					kind: "data",
				},
			],
		},

		[FRAME_BOARD]: FRAME,
	},
};
