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

import type { Project } from "@krudd/board";
import { cloneProject, Runner, TRIANGLES } from "@krudd/board";
import { mountBoardView } from "@krudd/board-view";
import { boot, fitCanvas, type World } from "@krudd/boundary";
import {
	loadFailure,
	PROJECT_ACCEPT,
	readProjectFile,
	saveFailure,
	saveProject,
} from "./project-file";
import { mountModeShell } from "./shell";

/** Where the canvas is. */
const CANVAS_ID = "viewport";

/** Where the readout goes. */
const STATUS_ID = "status";

/** Where a save or a load says what happened. */
const NOTICE_ID = "notice";

/** Where the board is drawn. */
const BOARD_ID = "board";

/** The save button, the open button, and the picker open hides behind. */
const SAVE_ID = "save";
const OPEN_ID = "open";
const FILE_ID = "file";

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
	// A copy, because the fixture is a module-level constant and editing the
	// board edits the project — the board is the source of truth, and a source
	// of truth shared with every other importer is not one.
	//
	// Both are `let` because a project can be opened from a file, which
	// replaces the pair. Everything below reads them through the closure, so a
	// load reaches the frame loop, the readout and the view without any of them
	// being handed anything.
	let project = cloneProject(TRIANGLES);
	let runner = new Runner(project, world);
	runner.start();

	// The board pane, filled. Drawn when the pane comes into view rather than
	// at boot: the view measures its nodes, and a node inside a pane that has
	// not been laid out yet measures zero.
	const view = mountBoardView({
		host: boardPane(),
		// The edit has already happened — the view changed the document, and the
		// document is what is running. This is only how the runner finds out, so
		// the change reaches the game on the very next frame with no reload.
		onEdit: () => runner.reload(),
	});
	let drawn = false;

	// After `boot`, and exactly once. The shell composites the two modes over
	// one canvas rather than routing between them, because the WebGL2 context
	// is taken for the life of the engine — see `shell.ts`.
	const shell = mountModeShell({
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
			view.open(project);
			drawn = true;
		},
	});

	/**
	 * Opens a project, in place of the one running.
	 *
	 * The order is the whole of it. `Runner` validates the document and refuses
	 * a kind that cannot run, so it is built *before* anything is touched: a
	 * file that does not hold together throws here, with the world still full
	 * of the board that was already running and the frame loop still walking
	 * it. Nothing is half-loaded, because nothing was loaded.
	 */
	const openProject = (next: Project): void => {
		const opened = new Runner(next, world);
		// Emptied, not stepped down entity by entity: a despawned slot is a
		// tombstone that still counts, and a `Start` lane that sizes its world
		// from `slotCount` — which `spawn ring` does — would find the old
		// board's slots already there and spawn nothing into them.
		world.clear();
		project = next;
		runner = opened;
		// The new board's `Start`, which is what spawns its world. Without it a
		// loaded project would be a board with nothing in front of it.
		runner.start();
		drawn = shell.mode === "board";
		if (drawn) {
			view.open(project);
		}
	};

	mountProjectControls({
		project: () => project,
		open: openProject,
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

/** How the project controls reach the project that is running. */
interface ProjectControls {
	/** The project as it stands, read when a save happens rather than held. */
	readonly project: () => Project;
	/** Opens one that came out of a file. Throws if it does not hold together. */
	readonly open: (project: Project) => void;
}

/** Binds save, open, the picker behind open, and dismissing what they said. */
function mountProjectControls(controls: ProjectControls): void {
	const chooser = required(FILE_ID);
	if (!(chooser instanceof HTMLInputElement)) {
		throw new Error(`#${FILE_ID} must be an <input type="file"> to open with`);
	}
	// From the package that decides what a project file is, rather than a
	// second copy of the extension written down here.
	chooser.accept = PROJECT_ACCEPT;

	required(SAVE_ID).addEventListener("click", () => {
		try {
			notice(`saved as ${saveProject(controls.project())}`);
		} catch (error) {
			// Noticed rather than latched: a download the browser would not take
			// has not broken the engine, and stopping the frame loop over it
			// would take the running board away too.
			notice(saveFailure(error), "bad");
		}
	});

	required(OPEN_ID).addEventListener("click", () => {
		chooser.click();
	});

	chooser.addEventListener("change", () => {
		const file = chooser.files?.[0];
		// Cleared either way, so that picking the same file twice is two
		// `change` events rather than one and then silence.
		chooser.value = "";
		if (file === undefined) {
			return;
		}
		void readProjectFile(file)
			.then((next) => {
				controls.open(next);
				notice(`opened ${file.name}`);
			})
			.catch((error: unknown) => {
				// Everything that could refuse has refused by now and the board
				// on screen is untouched, which is what the message says.
				notice(loadFailure(file.name, error), "bad");
			});
	});

	const said = required(NOTICE_ID);
	said.addEventListener("click", () => {
		said.hidden = true;
	});
}

/**
 * Says what a save or a load did.
 *
 * Its own element rather than the readout, which the frame loop rewrites sixty
 * times a second — a message painted over before it can be read is a message
 * that was never shown.
 */
function notice(message: string, kind: "good" | "bad" = "good"): void {
	const element = document.getElementById(NOTICE_ID);
	if (element === null) {
		return;
	}
	element.textContent = message;
	element.dataset.kind = kind;
	element.hidden = false;
}

/** An element the page is required to have. */
function required(id: string): HTMLElement {
	const element = document.getElementById(id);
	if (element === null) {
		throw new Error(`the page has no #${id} element`);
	}
	return element;
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
