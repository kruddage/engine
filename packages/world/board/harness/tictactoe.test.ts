// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Tic-tac-toe's rules (#866 PR-6), against a world that is nothing but named
 * columns — no wasm, no canvas, the same reason `run.test.ts` runs the
 * triangles project against `TestWorld`.
 *
 * The behaviour reproduced here is the pre-rewrite game's, not its Scheme:
 * `git show 81619e5^:krudd/engine/game/tictactoe/rules.scm` and its
 * `tictactoe_test.c` are the specification these tests port from a global
 * board vector and two Scheme functions to two node kinds and a pure helper.
 * Placement, turn alternation, winning, drawing and restart are asserted the
 * same way the C harness asserted them — a sequence of taps and a readout of
 * the board afterwards — just through `Runner` and `TestWorld` instead of
 * `scene_script_call` and a `struct world`.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import type { Board, Project } from "../src/index";
import {
	cloneProject,
	DOCUMENT_VERSION,
	KINDS,
	PAINT_TO_COLOURS,
	parseProject,
	Runner,
	rgbOf,
	serializeProject,
	TICTACTOE,
	TICTACTOE_BOARD,
	toggleWire,
	validate,
	winnerOf,
} from "../src/index";
import { TestWorld } from "./world";

/** The frame length every test steps at. Only its sign and non-zero-ness matter. */
const DT = 1 / 60;

/**
 * The board this suite runs: `spawn-grid` in the start lane, `place-mark` in
 * the step lane, an empty paint lane. `size` and every column-name param are
 * left at the kinds' own defaults, which is the configuration a project would
 * actually ship — restating them here would be exactly the noise
 * `triangles.ts`'s module docs warn a fixture that repeats defaults produces.
 */
function project(): Project {
	const root: Board = {
		title: "tic-tac-toe",
		columns: [
			{ name: "position", kind: "vec3" },
			{ name: "mark", kind: "u32" },
			{ name: "won", kind: "u32" },
			{ name: "turn", kind: "u32" },
			{ name: "over", kind: "u32" },
		],
		nodes: [
			{ id: "start", kind: "start", lane: "start", column: 0 },
			{ id: "spawn", kind: "spawn-grid", lane: "start", column: 1 },
			{ id: "step", kind: "step", lane: "step", column: 0 },
			{ id: "place", kind: "place-mark", lane: "step", column: 1 },
			{ id: "paint", kind: "paint", lane: "paint", column: 0 },
		],
		wires: [
			{
				id: "exec-start-spawn",
				from: { node: "start", port: "out" },
				to: { node: "spawn", port: "in" },
				kind: "exec",
			},
			{
				id: "exec-step-place",
				from: { node: "step", port: "out" },
				to: { node: "place", port: "in" },
				kind: "exec",
			},
		],
	};
	return { version: DOCUMENT_VERSION, root: "root", boards: { root } };
}

/**
 * The same board, minus the wire into `spawn-grid` — so `start()` still
 * ensures the four columns (per `Runner.start`'s own docs) but nothing ever
 * spawns, leaving them at zero length. This is the "before anything is
 * spawned" world the empty-world test needs, and it can only be built this
 * way: skipping `runner.start()` entirely would leave the columns not merely
 * empty but *uncreated*, which throws for a different reason than the one
 * under test.
 */
function projectWithNothingSpawned(): Project {
	const withSpawn = project();
	const root = withSpawn.boards.root;
	assert.ok(root !== undefined);
	return {
		...withSpawn,
		boards: {
			root: {
				...root,
				nodes: root.nodes.filter((n) => n.id !== "spawn"),
				wires: root.wires.filter((w) => w.id !== "exec-start-spawn"),
			},
		},
	};
}

/**
 * The world-space centre of cell `cell` (`row * 3 + col`) at the board's own
 * default `size` of 2 — the same arithmetic `spawn-grid` writes into
 * `position` and `pickCell` quantises against, restated here only so a test
 * can name a cell by number and get the point that lands on it.
 */
function cellCentre(cell: number, size = 2): { x: number; y: number } {
	const half = size / 2;
	const step = size / 3;
	const row = Math.floor(cell / 3);
	const col = cell % 3;
	return { x: -half + (col + 0.5) * step, y: half - (row + 0.5) * step };
}

/**
 * A tap at `cell`, in the normalised screen-space `PointerFrame` carries —
 * the inverse of `TestWorld`'s own `worldXFromScreen`/`worldYFromScreen`, at
 * its default camera (half-width and half-height both 2, `VIEW_EXTENT`). A
 * node under test never sees this inversion; it is only how a test states
 * "the finger came down here" in the units a `RunContext` actually hands out.
 */
function tapAt(
	cell: number,
	size = 2,
): { x: number; y: number; pressed: number } {
	const { x, y } = cellCentre(cell, size);
	const camera = { halfWidth: 2, halfHeight: 2 };
	return {
		x: (x / camera.halfWidth + 1) / 2,
		y: (1 - y / camera.halfHeight) / 2,
		pressed: 1,
	};
}

/** A screen point that unprojects well outside the grid, pressed or not. */
function tapOutsideTheGrid(): { x: number; y: number; pressed: number } {
	// Screen (0, 0), the viewport's top-left corner, unprojects to world
	// (-2, 2) at the default camera — outside the size-2 grid, which only
	// covers world x and y in (-1, 1).
	return { x: 0, y: 0, pressed: 1 };
}

/** The four columns this game plays on, read fresh — never cached across a tap. */
function state(world: TestWorld) {
	return {
		mark: world.column("mark") as Uint32Array,
		won: world.column("won") as Uint32Array,
		turn: world.column("turn") as Uint32Array,
		over: world.column("over") as Uint32Array,
	};
}

test("spawn-grid spawns the nine cells, row 0 at the top and cell 0 top-left", () => {
	const world = new TestWorld();
	new Runner(project(), world).start();

	assert.equal(world.slotCount, 9, "a 3x3 grid, no more and no fewer");
	const position = world.column("position") as Float32Array;
	// Cell 0's centre, at the default size of 2: top-left quadrant, x < 0
	// and y > 0 — the same convention `pick.ts` and `pick.test.ts` assert
	// against, restated here so a pick and a draw are checked against the
	// same source of truth rather than only against each other.
	assert.ok((position[0] as number) < 0 && (position[1] as number) > 0);
	// Cell 8's centre: bottom-right, x > 0 and y < 0.
	assert.ok((position[24] as number) > 0 && (position[25] as number) < 0);
});

test("running spawn-grid twice spawns the nine cells once, not eighteen", () => {
	const world = new TestWorld();
	const runner = new Runner(project(), world);
	runner.start();
	runner.start();
	assert.equal(world.slotCount, 9, "a second run must not spawn a second grid");
});

test("placing on an empty cell marks it and passes the turn to the other player", () => {
	const world = new TestWorld();
	const runner = new Runner(project(), world);
	runner.start();
	const { mark, turn, over } = state(world);

	runner.frame(DT, tapAt(0));
	assert.equal(mark[0], 1, "X landed on cell 0");
	assert.equal(turn[0], 2, "the turn passed to O");
	assert.equal(over[0], 0, "still playing");

	runner.frame(DT, tapAt(4));
	assert.equal(mark[4], 2, "O landed on cell 4, its own turn");
	assert.equal(turn[0], 1, "and the turn passed back to X");
});

test("tapping an already-marked cell is a complete no-op", () => {
	const world = new TestWorld();
	const runner = new Runner(project(), world);
	runner.start();
	const { mark, turn } = state(world);

	runner.frame(DT, tapAt(0));
	assert.equal(mark[0], 1);
	assert.equal(turn[0], 2);

	runner.frame(DT, tapAt(0));
	assert.equal(mark[0], 1, "still X — an occupied cell is not overwritten");
	assert.equal(turn[0], 2, "and the turn did not move either");
});

test("a press outside the grid, and a frame with no press at all, both do nothing", () => {
	const world = new TestWorld();
	const runner = new Runner(project(), world);
	runner.start();
	const { mark, turn } = state(world);

	runner.frame(DT, tapOutsideTheGrid());
	assert.deepEqual(Array.from(mark.slice(0, 9)), new Array(9).fill(0));
	assert.equal(turn[0], 0, "untouched — spawn-grid does not write turn either");

	// A frame with the default, un-pressed pointer sample — what every board
	// that never collects one gets, and what a hover without a tap looks
	// like to a node that does.
	runner.frame(DT);
	assert.deepEqual(Array.from(mark.slice(0, 9)), new Array(9).fill(0));
});

test("completing a line ends the round for that player without flipping the turn", () => {
	const world = new TestWorld();
	const runner = new Runner(project(), world);
	runner.start();
	const { mark, won, turn, over } = state(world);

	runner.frame(DT, tapAt(0)); // X
	runner.frame(DT, tapAt(3)); // O
	runner.frame(DT, tapAt(1)); // X
	runner.frame(DT, tapAt(4)); // O
	assert.equal(over[0], 0, "not decided until the completing move");

	runner.frame(DT, tapAt(2)); // X completes row 0-1-2
	assert.equal(over[0], 1, "X won");
	assert.deepEqual(
		[won[0], won[1], won[2]],
		[1, 1, 1],
		"the winning line is marked",
	);
	assert.deepEqual(
		[won[3], won[4], won[5], won[6], won[7], won[8]],
		[0, 0, 0, 0, 0, 0],
		"and no other cell is",
	);
	assert.equal(turn[0], 1, "the winning move does not flip the turn");
	assert.equal(mark[2], 1, "the winning cell itself was placed");
});

test("any tap once the round is over restarts to an empty board with X to move", () => {
	const world = new TestWorld();
	const runner = new Runner(project(), world);
	runner.start();
	const { mark, won, turn, over } = state(world);

	runner.frame(DT, tapAt(0)); // X
	runner.frame(DT, tapAt(3)); // O
	runner.frame(DT, tapAt(1)); // X
	runner.frame(DT, tapAt(4)); // O
	runner.frame(DT, tapAt(2)); // X wins
	assert.equal(over[0], 1);

	// The restarting tap is consumed by the restart itself — it places
	// nothing, matching the pre-rewrite `ttt-on-selected`, which calls
	// `ttt-restart` instead of `ttt-place-move` once the game is over.
	runner.frame(DT, tapAt(0));
	assert.equal(over[0], 0, "playing again");
	assert.equal(turn[0], 1, "X to move");
	assert.deepEqual(
		Array.from(mark.slice(0, 9)),
		new Array(9).fill(0),
		"the board is genuinely empty, not just reporting as not-over",
	);
	assert.deepEqual(Array.from(won.slice(0, 9)), new Array(9).fill(0));

	// And the board really is empty: X can play cell 0 anew.
	runner.frame(DT, tapAt(0));
	assert.equal(mark[0], 1);
	assert.equal(over[0], 0);
});

test("a full board with no line drawn settles as a draw, only on the move that fills it", () => {
	const world = new TestWorld();
	const runner = new Runner(project(), world);
	runner.start();
	const { over } = state(world);

	// X:0 O:1 X:2 O:4 X:3 O:5 X:8 O:6 X:7 ->
	//   X O X
	//   X O O
	//   O X X   (no three-in-a-row for either side)
	const moves = [0, 1, 2, 4, 3, 5, 8, 6, 7];
	for (const cell of moves.slice(0, 8)) {
		runner.frame(DT, tapAt(cell));
	}
	assert.equal(over[0], 0, "not decided until the last cell");

	const last = moves[8];
	assert.ok(last !== undefined);
	runner.frame(DT, tapAt(last));
	assert.equal(over[0], 3, "a draw");
});

test("a move that completes a line and fills the board at once is a win, never a draw", () => {
	const world = new TestWorld();
	const runner = new Runner(project(), world);
	runner.start();
	const { mark, over } = state(world);

	// X takes column 2-5-8; O takes the rest. X's fifth move, at cell 8, is
	// both the ninth placement (the board is then full) and the move that
	// completes the 2-5-8 column — worked out by hand so that no other line
	// completes earlier by coincidence.
	//   O X X
	//   O O X
	//   X O X
	const moves: readonly [number, "X" | "O"][] = [
		[1, "X"],
		[0, "O"],
		[2, "X"],
		[3, "O"],
		[5, "X"],
		[4, "O"],
		[6, "X"],
		[7, "O"],
		[8, "X"],
	];
	for (const [cell] of moves.slice(0, 8)) {
		runner.frame(DT, tapAt(cell));
	}
	assert.equal(over[0], 0, "eight cells filled, one line short of either");
	assert.equal(mark[8], 0, "the ninth cell is still open");

	const finalCell = moves[8]?.[0];
	assert.ok(finalCell !== undefined);
	runner.frame(DT, tapAt(finalCell));
	assert.equal(
		over[0],
		1,
		"a win, even though this move also fills the last empty cell",
	);
});

test("place-mark against a world with nothing spawned yet is a no-op, not a throw", () => {
	const world = new TestWorld();
	const runner = new Runner(projectWithNothingSpawned(), world);
	runner.start();
	assert.equal(world.slotCount, 0, "start ran, but nothing was spawned");

	assert.doesNotThrow(() => {
		runner.frame(DT, tapAt(0));
	});
	assert.equal(world.slotCount, 0, "still nothing to place a mark on");
});

test("winnerOf finds a winner along each of the eight lines individually", () => {
	const lines: readonly (readonly [number, number, number])[] = [
		[0, 1, 2],
		[3, 4, 5],
		[6, 7, 8],
		[0, 3, 6],
		[1, 4, 7],
		[2, 5, 8],
		[0, 4, 8],
		[2, 4, 6],
	];
	for (const line of lines) {
		for (const player of [1, 2] as const) {
			const marks = new Array(9).fill(0);
			for (const i of line) {
				marks[i] = player;
			}
			const result = winnerOf(marks);
			assert.equal(
				result.winner,
				player,
				`line [${line.join(",")}] should give player ${player}`,
			);
			assert.deepEqual(result.line, line);
		}
	}
});

// The project itself, from here down: the same rules, but through the board
// that actually ships rather than through this file's minimal fixture. What
// these add is everything the fixture leaves out — the paint lane, the colour
// column, and the params the shipped board sets rather than defaults.

/** The grid size the shipped project runs at, read from the document itself. */
function shippedSize(): number {
	const board = TICTACTOE.boards[TICTACTOE_BOARD];
	assert.ok(board !== undefined);
	const size = board.nodes.find((node) => node.id === "grid")?.params?.size;
	assert.equal(typeof size, "number");
	return size as number;
}

/** One cell's colour, as the three floats the column holds. */
function tintOf(world: TestWorld, cell: number): [number, number, number] {
	const colour = world.column("colour") as Float32Array;
	return [
		colour[cell * 3] as number,
		colour[cell * 3 + 1] as number,
		colour[cell * 3 + 2] as number,
	];
}

/** One of `colour-marks`'s own colour params, resolved through the registry. */
function tint(name: "empty" | "x" | "o" | "win"): [number, number, number] {
	const declared = KINDS["colour-marks"]?.params.find((p) => p.name === name);
	assert.ok(declared !== undefined, `colour-marks has no \`${name}\` param`);
	return [...rgbOf(declared.default as string)] as [number, number, number];
}

/**
 * That a cell holds a colour, to the precision the column actually keeps.
 *
 * The column is `f32` and `rgbOf` divides in double precision, so `#33384a`'s
 * red is 0.2 on one side and 0.20000000298023224 on the other. Comparing
 * exactly would be asserting that a `Float32Array` is not one.
 */
function hasTint(
	world: TestWorld,
	cell: number,
	expected: readonly [number, number, number],
	why: string,
): void {
	const held = tintOf(world, cell);
	for (let channel = 0; channel < 3; channel++) {
		assert.ok(
			Math.abs((held[channel] as number) - (expected[channel] as number)) <
				1e-6,
			`${why}: channel ${channel} is ${held[channel]}, expected ${expected[channel]}`,
		);
	}
}

test("the shipped project holds together, and opens on the board it names", () => {
	assert.deepEqual(validate(TICTACTOE), []);
	assert.ok(Object.hasOwn(TICTACTOE.boards, TICTACTOE.root));
	assert.equal(TICTACTOE.root, TICTACTOE_BOARD);
});

test("it survives the round trip a saved file takes, field for field", () => {
	const reopened = parseProject(serializeProject(TICTACTOE));
	assert.deepEqual(reopened, TICTACTOE, "the project did not survive the trip");
	assert.equal(
		serializeProject(reopened),
		serializeProject(TICTACTOE),
		"and saving what was opened gives the same bytes back",
	);
});

test("the grid it draws is the grid it picks against", () => {
	const board = TICTACTOE.boards[TICTACTOE_BOARD];
	assert.ok(board !== undefined);
	const sizes = board.nodes
		.filter((node) => node.kind === "spawn-grid" || node.kind === "place-mark")
		.map((node) => node.params?.size);
	assert.equal(sizes.length, 2, "one node lays the cells out, one picks them");
	assert.equal(
		sizes[0],
		sizes[1],
		"a pick that disagrees with the draw lands a tap on the wrong cell",
	);
});

test("it boots to nine cells, every one of them visible before anybody taps", () => {
	const world = new TestWorld();
	const runner = new Runner(TICTACTOE, world);
	runner.start();
	assert.equal(world.slotCount, 9);

	// The paint lane, once — which is where the tint is written, so before this
	// the colour column is whatever the engine filled it with.
	runner.frame(1 / 60);
	assert.equal(world.draws, 1, "the paint lane reached the renderer");
	for (let cell = 0; cell < 9; cell++) {
		hasTint(world, cell, tint("empty"), `cell ${cell} should read as empty`);
	}
});

test("playing it: an X, an O, and a lit winning line", () => {
	const world = new TestWorld();
	const runner = new Runner(TICTACTOE, world);
	const size = shippedSize();
	runner.start();

	runner.frame(1 / 60, tapAt(0, size));
	hasTint(world, 0, tint("x"), "X's cell took X's colour");
	hasTint(world, 1, tint("empty"), "and its neighbour did not");

	runner.frame(1 / 60, tapAt(3, size));
	hasTint(world, 3, tint("o"), "O's cell took O's colour");

	runner.frame(1 / 60, tapAt(1, size)); // X
	runner.frame(1 / 60, tapAt(4, size)); // O
	runner.frame(1 / 60, tapAt(2, size)); // X completes row 0-1-2

	const over = world.column("over") as Uint32Array;
	assert.equal(over[0], 1, "X won");
	for (const cell of [0, 1, 2]) {
		hasTint(world, cell, tint("win"), `cell ${cell} is on the winning line`);
	}
	hasTint(world, 3, tint("o"), "and a losing cell is not lit");
});

test("cutting the paint wire blanks the screen and the game plays on beneath it", () => {
	const world = new TestWorld();
	const project = cloneProject(TICTACTOE);
	assert.equal(toggleWire(project, TICTACTOE_BOARD, PAINT_TO_COLOURS), true);
	const runner = new Runner(project, world);
	const size = shippedSize();
	runner.start();

	runner.frame(1 / 60, tapAt(0, size));
	assert.equal(world.draws, 0, "nothing drew");
	assert.equal(
		world.blanks,
		1,
		"the screen was presented empty rather than stale",
	);

	const mark = world.column("mark") as Uint32Array;
	const turn = world.column("turn") as Uint32Array;
	assert.equal(mark[0], 1, "the tap still landed");
	assert.equal(
		turn[0],
		2,
		"and the turn still passed — the step lane is untouched",
	);
});

test("winnerOf reports no winner over an empty board or a board with no completed line", () => {
	assert.deepEqual(winnerOf(new Array(9).fill(0)), {
		winner: 0,
		line: undefined,
	});
	// X O X / O O X / O X O — no line either way.
	const drawish = [1, 2, 1, 2, 2, 1, 2, 1, 2];
	const result = winnerOf(drawish);
	assert.equal(result.winner, 0);
	assert.equal(result.line, undefined);
});
