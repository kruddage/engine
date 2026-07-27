// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * `pickCell`'s arithmetic: known world points against known cells.
 *
 * This is where the real coverage for `pick-grid` lives, per the module's own
 * docs — `pickCell` is pure and DOM-free, so it is tested directly rather than
 * through a running board. `cargo xtask render-test` still proves the
 * triangles demo, which has no `pick-grid` node, is unaffected.
 *
 * Run by `cargo xtask test-web`, which `cargo xtask check` runs.
 */

import assert from "node:assert/strict";
import test from "node:test";
import { pickCell } from "../src/pick";

/** The grid size every test below uses, unless it says otherwise. */
const SIZE = 3;

/** The centre of cell `(row, col)` at grid size [SIZE]. */
function centre(row: number, col: number): { x: number; y: number } {
	const half = SIZE / 2;
	const cell = SIZE / 3;
	return {
		x: -half + (col + 0.5) * cell,
		y: half - (row + 0.5) * cell,
	};
}

test("the nine cell centres each map to their own index", () => {
	for (let row = 0; row < 3; row++) {
		for (let col = 0; col < 3; col++) {
			const { x, y } = centre(row, col);
			const pick = pickCell(x, y, SIZE, 1);
			assert.equal(
				pick.cell,
				row * 3 + col,
				`row ${row}, col ${col} should be cell ${row * 3 + col}`,
			);
			assert.equal(pick.hit, 1);
		}
	}
});

test("cell 0 is the top-left, cell 8 the bottom-right", () => {
	// Not redundant with the loop above: this is the specific claim the brief
	// makes about parity with the pre-rewrite scene, asserted by name rather
	// than only by index arithmetic.
	assert.deepEqual(pickCell(centre(0, 0).x, centre(0, 0).y, SIZE, 1).cell, 0);
	assert.deepEqual(pickCell(centre(2, 2).x, centre(2, 2).y, SIZE, 1).cell, 8);
});

test("a point just outside the grid on any of the four sides misses", () => {
	const half = SIZE / 2;
	const epsilon = 0.01;
	const sides = [
		{ x: -half - epsilon, y: 0, label: "left" },
		{ x: half + epsilon, y: 0, label: "right" },
		{ x: 0, y: half + epsilon, label: "top" },
		{ x: 0, y: -half - epsilon, label: "bottom" },
	];
	for (const side of sides) {
		const pick = pickCell(side.x, side.y, SIZE, 1);
		assert.equal(pick.hit, 0, `${side.label} of the grid should miss`);
	}
});

test("an exact internal boundary belongs to the cell to its right or below", () => {
	const half = SIZE / 2;
	const third = SIZE / 3;

	// The vertical line between column 0 and column 1, at a row-1 height.
	const vertical = pickCell(-half + third, 0, SIZE, 1);
	assert.equal(vertical.cell, 4, "the column boundary resolves to column 1");

	// The horizontal line between row 0 and row 1, at a column-1 width.
	const horizontal = pickCell(0, half - third, SIZE, 1);
	assert.equal(horizontal.cell, 4, "the row boundary resolves to row 1");
});

test("the grid's own edges are closed top-left, open bottom-right", () => {
	const half = SIZE / 2;

	// The top-left corner, exactly on the grid's outer edge, is cell 0 — the
	// closed side of both axes.
	assert.equal(pickCell(-half, half, SIZE, 1).cell, 0);

	// The bottom-right corner sits exactly on the open side of both axes, so
	// it reads as outside the grid rather than as cell 8.
	assert.equal(
		pickCell(half, -half, SIZE, 1).hit,
		0,
		"the outer bottom-right edge is excluded, not clamped into cell 8",
	);
});

test("a press outside the grid misses; a hover inside it also misses", () => {
	const outside = pickCell(SIZE, SIZE, SIZE, 1);
	assert.equal(outside.hit, 0, "outside the grid, pressed or not, is a miss");

	const { x, y } = centre(1, 1);
	const hover = pickCell(x, y, SIZE, 0);
	assert.equal(
		hover.hit,
		0,
		"inside the grid without a press is a miss — hit needs both",
	);
});

test("a non-positive size never registers a hit", () => {
	assert.equal(pickCell(0, 0, 0, 1).hit, 0);
	assert.equal(pickCell(0, 0, -1, 1).hit, 0);
});
