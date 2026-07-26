// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The browser page's entry point.
 *
 * **Tier: `shell`.** It may reach for anything; nothing may reach for it. Its
 * `exports` map is empty, so importing it is a resolution error rather than a
 * convention violation.
 *
 * ## Scope
 *
 * Boot the engine against the canvas, drive one frame loop, and put a failure
 * on screen if there is one. That is #819's whole brief, and it is what this
 * file will keep doing once there is an editor above it: the chrome belongs in
 * data rather than here (#830).
 *
 * It also mounts the mode shell — the two panes the page swipes between, the
 * game and the board. That stays here rather than moving up a tier because it
 * is the *host's* job to composite: the canvas is booted once and never
 * unmounted, and `shell.ts` is what guarantees it.
 *
 * The loop exercises the boundary rule in both directions. Rust draws;
 * TypeScript reads the position column out of wasm memory with no copy and
 * writes back into that same view — a whole-world edit with no call across the
 * boundary at all.
 *
 * ## The demo is data now
 *
 * What used to be `populate` and `recycle` here is the triangles project, a
 * board in `@krudd/board`. This file no longer knows that there are eight
 * entities, that they spawn on a circle of 0.4, or that they drift at 0.25 —
 * the document does, and `Runner` walks it. The page's job is to boot the
 * engine, run the board once at open and once a frame, and put a failure on
 * screen. Which is what it was before; there is simply less of it.
 */

import { type Board, Runner, TRIANGLES } from "@krudd/board";
import { mountBoardView } from "@krudd/board-view";
import { boot, fitCanvas, type World } from "@krudd/boundary";
import { mountModeShell } from "./shell";

/** Where the canvas is. */
const CANVAS_ID = "viewport";

/** Where the readout goes. */
const STATUS_ID = "status";

/** Where the board is drawn. */
const BOARD_ID = "board";

/**
 * Whether a failure has already been reported.
 *
 * Latched, and checked by the frame loop before it writes the readout: a
 * failure raised outside the loop — a resize handler, a rejected promise — must
 * not be painted over by the next frame's status line. An error the user saw
 * for 16ms is an error nobody saw.
 */
let failed = false;

/** Boots the engine and starts the frame loop. */
async function main(): Promise<void> {
	const canvas = document.getElementById(CANVAS_ID);
	if (!(canvas instanceof HTMLCanvasElement)) {
		throw new Error(`the page has no <canvas id="${CANVAS_ID}"> to draw into`);
	}
	const status = statusElement();

	const krudd = await boot({ canvas });
	const world = krudd.world;
	// Refused here, loudly, rather than a frame at a time: a document that does
	// not hold together must not boot into a board that half works.
	const runner = new Runner(TRIANGLES, world);
	runner.start();

	// The board pane, filled. Drawn when the pane comes into view rather than
	// at boot: the view measures its nodes, and a node inside a pane that has
	// not been laid out yet measures zero.
	const view = mountBoardView({ host: boardPane() });
	let drawn = false;

	// After `boot`, and exactly once. The shell composites the two modes over
	// one canvas rather than routing between them, because the WebGL2 context
	// is taken for the life of the engine — see `shell.ts`.
	mountModeShell({
		canvas,
		onFail: fail,
		onChange: (mode) => {
			if (mode !== "board") {
				return;
			}
			if (drawn) {
				view.refresh();
				return;
			}
			view.show(rootBoard());
			drawn = true;
		},
	});

	// Not debounced: `fitCanvas` returns false when nothing changed, so a resize
	// storm costs a couple of property reads per event rather than a swapchain
	// reconfiguration each time.
	window.addEventListener("resize", () => {
		if (fitCanvas(canvas)) {
			world.resize(canvas.width, canvas.height);
		}
	});

	let previous = performance.now();
	const frame = (now: number): void => {
		if (failed) {
			return;
		}
		// Clamp the step: a backgrounded tab resumes with a delta measured in
		// seconds, and integrating that in one go teleports everything.
		const dt = Math.min((now - previous) / 1000, 0.1);
		previous = now;
		try {
			// Step then paint, both from the document. Nothing about what
			// happens in either is decided here.
			runner.frame(dt);
		} catch (error) {
			// Reported once and the loop stops. A frame that failed will fail
			// again next frame, and a page reporting the same error sixty times a
			// second is a page nobody can read the first one on.
			fail(error);
			return;
		}
		status.textContent = report(krudd.version, world, runner, dt);
		requestAnimationFrame(frame);
	};
	requestAnimationFrame(frame);
}

/** One line of readout per frame. */
function report(
	version: string,
	world: World,
	runner: Runner,
	dt: number,
): string {
	const fps = dt > 0 ? (1 / dt).toFixed(0) : "—";
	const { width, height } = world.viewport;
	return [
		`krudd ${version}`,
		world.rendererDescription ?? "no renderer",
		`${width}x${height}`,
		`${fps} fps`,
		`${world.entityCount} entities`,
		`${world.drawCount} draws`,
		// The board's count and clock, not the engine's: the engine's `tick`
		// integrates a column, and integrating is a node now.
		`frame ${runner.frameCount}`,
		`${runner.elapsed.toFixed(1)}s`,
	].join("  ·  ");
}

/** The board pane, which the page is required to have. */
function boardPane(): HTMLElement {
	const element = document.getElementById(BOARD_ID);
	if (element === null) {
		throw new Error(
			`the page has no #${BOARD_ID} element to draw the board in`,
		);
	}
	return element;
}

/** The board the triangles project opens on. */
function rootBoard(): Board {
	const board = TRIANGLES.boards[TRIANGLES.root];
	if (board === undefined) {
		throw new Error(
			`the project opens on board \`${TRIANGLES.root}\`, which it does not hold`,
		);
	}
	return board;
}

/** The readout element, which the page is required to have. */
function statusElement(): HTMLElement {
	const element = document.getElementById(STATUS_ID);
	if (element === null) {
		throw new Error(`the page has no #${STATUS_ID} element to report into`);
	}
	return element;
}

/**
 * Puts a failure on the page, not only in the console.
 *
 * A boot failure that only reaches the console is indistinguishable from a page
 * that rendered nothing on purpose — and a render failure is worse, because the
 * last frame stays on the canvas and the page goes on looking like it works.
 * Both end up here.
 */
function fail(error: unknown): void {
	failed = true;
	const message = error instanceof Error ? error.message : String(error);
	const element = document.getElementById(STATUS_ID);
	if (element !== null) {
		element.textContent = `krudd failed — ${message}`;
		element.classList.add("failed");
	}
	console.error(error);
}

main().catch(fail);
