// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Tic-tac-toe win detection over nine marks (#866 PR-6).
 *
 * Pure and free of the world, exactly as `./pick`'s `pickCell` is free of the
 * DOM: it is arithmetic over nine numbers, so it can be asserted under
 * `node:test` against known boards rather than only by playing a full game
 * through `place-mark`.
 *
 * There is no `detect-win` *node* — see `kinds.ts`'s module docs, above
 * `place-mark`, for why. This is that function anyway, factored out on its
 * own so it can be exported and tested separately from the move it is one
 * step of.
 */

/** One cell's mark: 0 empty, 1 X, 2 O. */
export type Mark = 0 | 1 | 2;

/** What scanning the board for a winner found. */
export interface WinResult {
	/** 1 or 2 if that player holds a full line, else 0. */
	readonly winner: Mark;
	/**
	 * The winning triple of cell indices, or `undefined` while there is none.
	 *
	 * Kept apart from `winner === 0` doubling as the signal, so a caller that
	 * only wants to know *whether* someone won never has to destructure a
	 * value that is meaningless the rest of the time — the same shape
	 * `pick.ts`'s `GridPick` uses for `cell` versus `hit`.
	 */
	readonly line: readonly [number, number, number] | undefined;
}

/**
 * The eight winning lines, in the exact order the pre-rewrite Scheme
 * (`*ttt-lines*` in `krudd/engine/game/tictactoe/rules.scm`) scanned them in:
 * three rows, three columns, then the two diagonals.
 *
 * The order cannot change which line a board reports — nine marks placed one
 * at a time can never complete two lines in the same move — but scanning in
 * the spec's own order keeps a diff against it legible, which is the point of
 * reproducing behaviour rather than re-deriving it.
 */
const LINES: readonly (readonly [number, number, number])[] = [
	[0, 1, 2],
	[3, 4, 5],
	[6, 7, 8],
	[0, 3, 6],
	[1, 4, 7],
	[2, 5, 8],
	[0, 4, 8],
	[2, 4, 6],
];

/**
 * Scans the nine marks for a completed line.
 *
 * `marks` is read only at indices 0 through 8 — exactly the nine cells
 * `spawn-grid` spawns and `place-mark` writes into — so a `Uint32Array`
 * column and a plain test array both satisfy it with no copy in between.
 */
export function winnerOf(marks: ArrayLike<number>): WinResult {
	for (const line of LINES) {
		const [a, b, c] = line;
		const mark = marks[a] as number;
		if (
			mark !== 0 &&
			mark === (marks[b] as number) &&
			mark === (marks[c] as number)
		) {
			return { winner: mark as Mark, line };
		}
	}
	return { winner: 0, line: undefined };
}
