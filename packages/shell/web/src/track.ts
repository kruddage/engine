// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The track: two modes, and the rules for moving between them.
 *
 * Deliberately free of the DOM. Everything here is arithmetic over a
 * pointer's position and a key name, which means the rules that decide
 * whether a drag navigates or pans can be asserted under `node:test` rather
 * than only by hand on a phone. `shell.ts` is the half that touches
 * elements.
 *
 * ## The rule that matters
 *
 * **A drag that starts at the edge navigates; a drag that starts anywhere
 * else is content.** The board is panned by dragging it, and a board that
 * threw you into the game every time you moved it sideways would be a board
 * nobody could use. So the gesture is decided once, by where it began, and
 * never re-decided part way through — a drag that started in the middle
 * stays content however far it travels.
 */

/**
 * The modes, in the order they sit on the track.
 *
 * Two, and that is the whole list: the game, and the board that makes it.
 * More can come later; a pause mode is explicitly not one of them yet.
 */
export const MODES = ["game", "board"] as const;

/** Which mode is showing. */
export type Mode = (typeof MODES)[number];

/**
 * How wide the navigating edge is, in CSS pixels.
 *
 * Thumb-sized rather than pixel-precise, because the device this is designed
 * for is held rather than pointed at.
 */
export const EDGE_WIDTH = 32;

/**
 * How far a navigating drag must travel before the mode changes.
 *
 * Far enough that resting a thumb on the edge of a phone does not switch
 * mode on its own.
 */
export const SWIPE_DISTANCE = 56;

/** Whether a string is one of the modes. */
export function isMode(value: string): value is Mode {
	return (MODES as readonly string[]).includes(value);
}

/**
 * The mode one step along the track, or `undefined` at either end.
 *
 * The track does not wrap. Wrapping would mean swiping the same way twice
 * returns you to where you started, which reads as the gesture having failed
 * rather than as having run out of track.
 */
export function neighbour(mode: Mode, direction: -1 | 1): Mode | undefined {
	return MODES[MODES.indexOf(mode) + direction];
}

/**
 * Whether a drag beginning at `x` across a track `width` wide navigates.
 *
 * Either edge, because both modes have a neighbour in one direction or the
 * other and a gesture that only worked from the right would have to be
 * learned twice.
 */
export function grabsEdge(x: number, width: number): boolean {
	return x <= EDGE_WIDTH || x >= width - EDGE_WIDTH;
}

/**
 * Where a navigating drag of `dx` from `from` lands, or `undefined` if it has
 * not travelled far enough or there is no track that way.
 *
 * Dragging left reveals what is to the right, the way a sheet of paper moves
 * under a finger.
 */
export function swipeTarget(from: Mode, dx: number): Mode | undefined {
	if (Math.abs(dx) < SWIPE_DISTANCE) {
		return undefined;
	}
	return neighbour(from, dx < 0 ? 1 : -1);
}

/**
 * Which way an arrow key moves along the track, or `undefined` for a key that
 * is not one.
 *
 * The keyboard is not an afterthought here: it is how the editor is driven on
 * a desktop, and how a test drives it without synthesising touches.
 */
export function keyDirection(key: string): -1 | 1 | undefined {
	if (key === "ArrowRight") {
		return 1;
	}
	if (key === "ArrowLeft") {
		return -1;
	}
	return undefined;
}
