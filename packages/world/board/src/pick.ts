// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Quantising a world-space point to a 3×3 grid cell.
 *
 * Deliberately free of the DOM and of the wasm boundary, exactly as
 * `packages/shell/web/src/pointer.ts` is free of the DOM: everything here is
 * arithmetic over two floats and a size, which means it can be asserted under
 * `node:test` against known points rather than only by tapping a phone.
 *
 * ## Where the world coordinates come from
 *
 * This module never unprojects a screen coordinate itself. `Engine::
 * world_x_from_screen` and `Engine::world_y_from_screen` in
 * `crates/shell/web/src/lib.rs` are the *only* code that knows the camera's
 * matrix, because that matrix is also what `build_frame` draws with — a
 * second, TypeScript-side copy of that arithmetic is exactly the way picking
 * and drawing end up disagreeing at some aspect ratio. So `pickCell` takes
 * world units in, already unprojected, and has no aspect ratio or viewport
 * size of its own to get wrong.
 *
 * ## The grid
 *
 * Centred on the world origin, `size` world units wide and tall. Cell 0 is
 * top-left on screen — row 0 at the top, matching the pre-rewrite scene,
 * where `cell-0` sat at the far corner and read as the top-left square — and
 * cell 8 is bottom-right, `index = row * 3 + col`.
 */

/** What one pick against the grid resolves to. */
export interface GridPick {
	/**
	 * Which of the nine cells the point falls in, `row * 3 + col`.
	 *
	 * Meaningless when `hit` is 0 — a point outside the grid still reports
	 * `0` here rather than a sentinel, because the output port is `u32` and
	 * has no value reserved to mean "nowhere". Callers must check `hit`
	 * first, the same way a node reading `c.pointer` has to check `pressed`
	 * before trusting `x`/`y` mattered.
	 */
	readonly cell: number;
	/** 1 when the point is inside the grid and the pointer is pressed; 0 otherwise. */
	readonly hit: 0 | 1;
}

/**
 * Quantises a world-space point `(x, y)` to one of nine cells of a `size` ×
 * `size` grid centred on the origin, and reports whether that is a press
 * worth acting on.
 *
 * `pressed` is `RunContext.pointer.pressed` — the edge-triggered value
 * `packages/shell/web/src/pointer.ts` already debounces to one frame — passed
 * straight through rather than re-decided here, so there is exactly one place
 * in the engine that turns a held finger into a single press.
 *
 * ## The boundary convention, and the tie it breaks
 *
 * Each axis is closed on its low side and open on its high side: a cell owns
 * its starting coordinate and excludes the coordinate where the next one
 * begins. Concretely, the grid covers `x` in `[-size/2, size/2)` and, since
 * world `y` runs up while row 0 is the top, `y` in `(size/2 - size, size/2]`
 * read the other way — the top edge `y = size/2` is closed (row 0), the
 * bottom edge `y = -size/2` is open (outside the grid, not row 2). A point
 * that lands exactly on an internal grid line therefore belongs to the cell
 * to its right or below, not the one to its left or above; a point exactly
 * on the grid's own bottom or right edge is outside it. This is one
 * consistent rule rather than a special case at the outer edge, and it is
 * what `floor` gives for free once the coordinate has been normalised to
 * `[0, 1)`.
 *
 * A non-positive `size` never registers a hit — `pick-grid`'s `size` param
 * has a `min` of 0, so this only guards a param that has been driven to its
 * floor, not a case the validator would otherwise let through.
 */
export function pickCell(
	x: number,
	y: number,
	size: number,
	pressed: number,
): GridPick {
	const miss: GridPick = { cell: 0, hit: 0 };
	if (!(size > 0)) {
		return miss;
	}
	const half = size / 2;
	// u: 0 at the left edge, 1 at the right. v: 0 at the top edge (world
	// `+half`, since world y is up and row 0 is the top), 1 at the bottom.
	const u = (x + half) / size;
	const v = (half - y) / size;
	const col = Math.floor(u * 3);
	const row = Math.floor(v * 3);
	if (col < 0 || col > 2 || row < 0 || row > 2) {
		return miss;
	}
	return { cell: row * 3 + col, hit: pressed === 1 ? 1 : 0 };
}
