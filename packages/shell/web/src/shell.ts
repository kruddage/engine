// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The mode shell: swiping between the game and the board.
 *
 * ## The invariant this file exists to keep
 *
 * **The canvas is created once and never unmounted.** `boot()` takes a canvas
 * for the life of the engine, and a second boot against the same canvas fails
 * outright — the WebGL2 context is already taken and a canvas only ever has
 * one. So mode switching here is *compositing*: both panes stay in the
 * document at their full size for the whole life of the page, and the board
 * slides over the game. A router that unmounted the viewport would work
 * perfectly once and kill the engine on the second swipe, which is the exact
 * shape of bug that is easy to write and miserable to find.
 *
 * The rule is asserted rather than trusted. Every mode change checks that the
 * canvas is still connected and still under the parent it started in, and
 * fails loudly if it is not — a canvas that quietly left the page presents as
 * a game that stopped drawing, and #812's rule is that an instrument may
 * never report "broken" and "fine" the same way.
 *
 * The game pane also keeps its size in board mode rather than being hidden.
 * `fitCanvas` sizes the drawing buffer from `clientWidth`, and a hidden pane
 * measures zero — the buffer would collapse to 1x1 on the next resize and
 * come back a blurry mess.
 */

import {
	grabsEdge,
	isMode,
	keyDirection,
	type Mode,
	neighbour,
	swipeTarget,
} from "./track";

/** The element the panes slide within, and where the gestures are read. */
const TRACK_ID = "track";

/** The bottom rail: one button per mode. */
const RAIL_ID = "modes";

/** A mounted shell. */
export interface ModeShell {
	/** Which mode is showing. */
	readonly mode: Mode;
	/** Shows a mode, as the rail does. */
	show(mode: Mode): void;
}

/** How the shell reports a failure it cannot itself recover from. */
export interface ShellOptions {
	/**
	 * The canvas the engine has already been booted against.
	 *
	 * Passed in rather than looked up so that the shell holds the same
	 * element the engine does, which is the thing it guards.
	 */
	readonly canvas: HTMLCanvasElement;
	/**
	 * Called after the mode changes, and once on mount.
	 *
	 * The board view measures its nodes, and a node inside a pane that has not
	 * been laid out measures zero — so whatever fills a pane needs to know when
	 * that pane came into view. Cheaper and far more honest than the page
	 * guessing from the same pointer and key events the shell is already
	 * reading.
	 */
	readonly onChange?: (mode: Mode) => void;
	/**
	 * Where a failure inside an event handler goes.
	 *
	 * Handlers run outside `main`'s `catch`, so a throw from one would reach
	 * nothing but the console. The page's own failure path takes it instead.
	 */
	readonly onFail: (error: unknown) => void;
}

/**
 * Binds the rail, the edge swipe and the arrow keys, and shows the game.
 *
 * Returns the shell so the page can drive it too; everything it does is
 * something a person could have done through the rail.
 */
export function mountModeShell(options: ShellOptions): ModeShell {
	const track = required(TRACK_ID);
	const rail = required(RAIL_ID);
	const canvas = options.canvas;
	// Where the canvas started. Compared on every switch rather than merely
	// remembered — see the module docs.
	const home = canvas.parentElement;
	if (home === null) {
		throw new Error(
			"the canvas is not in the page, so there is nothing to composite",
		);
	}

	let mode: Mode = "game";
	/** The gesture in flight, if it is one that navigates. */
	let swipe: { pointer: number; from: Mode; startX: number } | null = null;

	const show = (next: Mode): void => {
		mode = next;
		document.body.dataset.mode = next;
		for (const button of rail.querySelectorAll("button")) {
			const owned = button.dataset.mode === next;
			button.setAttribute("aria-current", owned ? "true" : "false");
		}
		if (!canvas.isConnected || canvas.parentElement !== home) {
			throw new Error(
				"the canvas left the page on a mode switch — the WebGL2 context is " +
					"taken for the life of the engine and cannot be created again",
			);
		}
		options.onChange?.(next);
	};

	/** `show`, for the paths that run outside `main`'s `catch`. */
	const guarded = (next: Mode): void => {
		try {
			show(next);
		} catch (error) {
			options.onFail(error);
		}
	};

	track.addEventListener("pointerdown", (event) => {
		// Decided once, here, and never re-decided: a drag that began in the
		// middle of a board is panning that board however far it travels.
		if (!grabsEdge(event.clientX, track.clientWidth)) {
			return;
		}
		swipe = { pointer: event.pointerId, from: mode, startX: event.clientX };
		track.setPointerCapture(event.pointerId);
	});

	track.addEventListener("pointermove", (event) => {
		if (swipe === null || swipe.pointer !== event.pointerId) {
			return;
		}
		const target = swipeTarget(swipe.from, event.clientX - swipe.startX);
		if (target !== undefined && target !== mode) {
			guarded(target);
		}
	});

	const release = (event: PointerEvent): void => {
		if (swipe?.pointer === event.pointerId) {
			swipe = null;
		}
	};
	track.addEventListener("pointerup", release);
	track.addEventListener("pointercancel", release);

	rail.addEventListener("click", (event) => {
		const target = event.target;
		const button = target instanceof Element ? target.closest("button") : null;
		const wanted = button?.dataset.mode;
		if (wanted !== undefined && isMode(wanted)) {
			guarded(wanted);
		}
	});

	window.addEventListener("keydown", (event) => {
		const direction = keyDirection(event.key);
		if (direction === undefined) {
			return;
		}
		const target = neighbour(mode, direction);
		if (target === undefined) {
			return;
		}
		// Only once there is somewhere to go: an arrow at the end of the track
		// should still scroll whatever is under it.
		event.preventDefault();
		guarded(target);
	});

	show(mode);
	return {
		get mode(): Mode {
			return mode;
		},
		show: guarded,
	};
}

/** An element the page is required to have. */
function required(id: string): HTMLElement {
	const element = document.getElementById(id);
	if (element === null) {
		throw new Error(`the page has no #${id} element for the mode shell`);
	}
	return element;
}
