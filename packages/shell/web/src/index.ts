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
 * It now also mounts the mode shell — the two panes the page swipes between,
 * the game and the board. That stays here rather than moving up a tier
 * because it is the *host's* job to composite: the canvas is booted once and
 * never unmounted, and `shell.ts` is what guarantees it.
 *
 * The loop exercises the boundary rule in both directions. Rust simulates and
 * draws; TypeScript reads the position column out of wasm memory with no copy,
 * and writes back into that same view to recycle entities that have drifted out
 * of frame — a whole-world edit with no call across the boundary at all.
 */

import { type Board, TRIANGLES } from "@krudd/board";
import { mountBoardView } from "@krudd/board-view";
import { boot, fitCanvas, type World } from "@krudd/boundary";
import { mountModeShell } from "./shell";

/** How many entities the demo spawns. */
const ENTITY_COUNT = 8;

/** The radius entities are spawned and recycled onto, in world units. */
const SPAWN_RADIUS = 0.4;

/** How fast they drift outward, in world units per second. */
const DRIFT_SPEED = 0.25;

/**
 * How far an entity may drift before it is put back.
 *
 * Comfortably outside the camera's box, so a recycle happens off screen rather
 * than as a visible jump.
 */
const RECYCLE_RADIUS = 3.2;

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
	populate(world);

	// After `boot`, and exactly once. The shell composites the two modes over
	// one canvas rather than routing between them, because the WebGL2 context
	// is taken for the life of the engine — see `shell.ts`.
	// The board pane, filled. Drawn when the pane comes into view rather than
	// at boot: the view measures its nodes, and a node inside a pane that has
	// not been laid out yet measures zero.
	const view = mountBoardView({ host: boardPane() });
	let drawn = false;
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
			world.tick(dt);
			recycle(world);
			world.render();
		} catch (error) {
			// Reported once and the loop stops. A frame that failed will fail
			// again next frame, and a page reporting the same error sixty times a
			// second is a page nobody can read the first one on.
			fail(error);
			return;
		}
		status.textContent = report(krudd.version, world, dt);
		requestAnimationFrame(frame);
	};
	requestAnimationFrame(frame);
}

/** Spawns the demo entities on a circle, each drifting outward. */
function populate(world: World): void {
	for (let i = 0; i < ENTITY_COUNT; i++) {
		const angle = (i / ENTITY_COUNT) * Math.PI * 2;
		const slot = world.spawn(
			Math.cos(angle) * SPAWN_RADIUS,
			Math.sin(angle) * SPAWN_RADIUS,
			0,
		);
		world.setVelocity(
			slot,
			Math.cos(angle) * DRIFT_SPEED,
			Math.sin(angle) * DRIFT_SPEED,
			0,
		);
	}
}

/**
 * Puts entities that have drifted out of frame back onto the spawn circle,
 * keeping their direction so they set off along the same ray.
 *
 * Written straight into the position column: the `Float32Array` is a view over
 * wasm linear memory, so this edits Rust's own state with no crossing per
 * entity and none at all beyond fetching the view. `setPosition` would reach
 * the same bytes at one call each, and `docs/boundary.md` has the measurement
 * of what that costs.
 */
function recycle(world: World): void {
	const positions = world.positions();
	for (let slot = 0; slot < world.slotCount; slot++) {
		const i = slot * 3;
		const x = positions[i] as number;
		const y = positions[i + 1] as number;
		const distance = Math.hypot(x, y);
		if (distance <= RECYCLE_RADIUS) {
			continue;
		}
		const scale = SPAWN_RADIUS / distance;
		positions[i] = x * scale;
		positions[i + 1] = y * scale;
	}
}

/** One line of readout per frame. */
function report(version: string, world: World, dt: number): string {
	const fps = dt > 0 ? (1 / dt).toFixed(0) : "—";
	const { width, height } = world.viewport;
	return [
		`krudd ${version}`,
		world.rendererDescription ?? "no renderer",
		`${width}x${height}`,
		`${fps} fps`,
		`${world.entityCount} entities`,
		`${world.drawCount} draws`,
		`frame ${world.frameCount}`,
		`${world.elapsed.toFixed(1)}s`,
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
