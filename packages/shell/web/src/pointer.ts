// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The pointer source: turning DOM events into what the graph reads.
 *
 * Deliberately free of the DOM, exactly as `track.ts` is — everything here is
 * arithmetic over a position, a viewport size and a down/up transition, which
 * means it can be asserted under `node:test` rather than only by tapping a
 * phone. `index.ts` is the half that touches elements: it owns a
 * [`PointerTrack`], feeds it real `pointerdown` / `pointermove` / `pointerup`
 * events, and samples it once per rendered frame.
 *
 * ## The two rules that matter
 *
 * **Edge-triggered.** `pressed` reads 1 for exactly the one `sample()` that
 * follows a press, however many frames the finger or button stays down after
 * that. The pre-rewrite engine debounced on a `g_last_sel` comparison for
 * exactly this reason: without the edge, a held press would place on every
 * frame it stays down rather than once — filling a tic-tac-toe board in a
 * single frame.
 *
 * **The edge-swipe zone never reaches the graph.** A press that begins inside
 * `grabsEdge`'s zone is the mode shell's gesture, not the game's — `track.ts`
 * already draws that line at `EDGE_WIDTH`, and this reuses the exact same
 * predicate rather than a second guess at where the boundary is. Decided once,
 * at the moment the pointer goes down, and never re-decided as it moves — the
 * same rule `track.ts` itself follows for the same reason.
 *
 * ## Coordinates
 *
 * `x` and `y` are viewport-relative and axis-independent: `x = clientX /
 * width`, `y = clientY / height`, each in `[0, 1]` with `(0, 0)` at the
 * top-left — the DOM's own convention, so nothing is flipped crossing in.
 * Dividing each axis by its own extent, rather than by one shared scale, is
 * what keeps a resize from moving where a tap lands and keeps a non-square
 * viewport from skewing it: the centre of the pane reads `(0.5, 0.5)` whether
 * the pane is a square or a phone screen turned on its side.
 */

import { grabsEdge } from "./track";

/** One pointer sample the graph reads. Structurally `@krudd/board`'s `PointerFrame`. */
export interface PointerFrame {
	readonly x: number;
	readonly y: number;
	readonly pressed: 0 | 1;
}

/** Nothing pressed, and nowhere in particular — before any event has arrived. */
export const NO_POINTER: PointerFrame = { x: 0, y: 0, pressed: 0 };

/**
 * The pointer, tracked across DOM events and sampled once a frame.
 *
 * One instance for the life of the page, held by `index.ts` the way `shell.ts`
 * holds `swipe` — state that persists between events but belongs to no single
 * one of them.
 */
export class PointerTrack {
	#x = 0;
	#y = 0;
	/** Whether a press is down that the graph is tracking (not an edge swipe). */
	#down = false;
	/**
	 * Whether the gesture in flight began in the edge-swipe zone.
	 *
	 * Latched at `down()` and never reconsidered — a drag that started at the
	 * edge stays the mode shell's however far it travels, exactly the rule
	 * `track.ts` applies to its own gesture.
	 */
	#ignoring = false;
	/** Whether a press has happened since the last `sample()`. Consumed by it. */
	#justPressed = false;

	/**
	 * A pointer going down at `(x, y)` in a viewport `width` × `height`.
	 *
	 * Ignored — the graph never sees it, not even its coordinate — if it began
	 * in the zone `grabsEdge` claims for navigation. `width` is the same
	 * measurement `track.ts`'s own `grabsEdge` calls are made with, so the two
	 * boundaries cannot disagree.
	 */
	down(x: number, y: number, width: number, height: number): void {
		if (grabsEdge(x, width)) {
			this.#ignoring = true;
			this.#down = false;
			return;
		}
		this.#ignoring = false;
		this.#down = true;
		this.#justPressed = true;
		this.#place(x, y, width, height);
	}

	/** A pointer moving to `(x, y)`. Updates the position only; a move never presses. */
	move(x: number, y: number, width: number, height: number): void {
		if (!this.#down || this.#ignoring) {
			return;
		}
		this.#place(x, y, width, height);
	}

	/** A pointer lifting, or its gesture cancelling. Ends the press. */
	up(): void {
		this.#down = false;
		this.#ignoring = false;
	}

	/**
	 * One frame's read.
	 *
	 * Call this once per rendered frame, never twice: it consumes the edge, so
	 * a second call in the same frame would read the press as already spent.
	 * `pressed` is 1 only the first time this is called after a `down()` that
	 * was not ignored.
	 */
	sample(): PointerFrame {
		const pressed = this.#justPressed ? 1 : 0;
		this.#justPressed = false;
		return { x: this.#x, y: this.#y, pressed };
	}

	#place(x: number, y: number, width: number, height: number): void {
		this.#x = width > 0 ? x / width : 0;
		this.#y = height > 0 ? y / height : 0;
	}
}
