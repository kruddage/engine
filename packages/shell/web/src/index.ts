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
 * This is proof that the two halves are wired together, not the engine's
 * shell. It renders no pixels — the WebGL2 renderer is #818 and the real HTML
 * shell is #819. What it does is exercise the whole chain once per frame:
 * Rust simulates, TypeScript reads the result out of wasm memory with no
 * copy, and the page shows a number that changes.
 */

import { boot, type World } from "@krudd/boundary";

/** How many entities the demo spawns. */
const ENTITY_COUNT = 8;

/** Where the status text goes. */
const STATUS_ID = "status";

/** Boots the engine and starts the frame loop. */
async function main(): Promise<void> {
	const status = document.getElementById(STATUS_ID);
	if (status === null) {
		throw new Error(`the page has no #${STATUS_ID} element to report into`);
	}

	const krudd = await boot({
		width: window.innerWidth,
		height: window.innerHeight,
	});
	const world = krudd.world;
	populate(world);

	window.addEventListener("resize", () => {
		world.resize(window.innerWidth, window.innerHeight);
	});

	let previous = performance.now();
	const frame = (now: number): void => {
		// Clamp the step: a backgrounded tab resumes with a delta measured in
		// seconds, and integrating that in one go teleports everything.
		const dt = Math.min((now - previous) / 1000, 0.1);
		previous = now;
		world.tick(dt);
		status.textContent = report(krudd.version, world, dt);
		requestAnimationFrame(frame);
	};
	requestAnimationFrame(frame);
}

/** Spawns the demo entities on a circle, each drifting outward. */
function populate(world: World): void {
	for (let i = 0; i < ENTITY_COUNT; i++) {
		const angle = (i / ENTITY_COUNT) * Math.PI * 2;
		const slot = world.spawn(Math.cos(angle), Math.sin(angle), 0);
		world.setVelocity(slot, Math.cos(angle) * 0.1, Math.sin(angle) * 0.1, 0);
	}
}

/** One line of status per frame. */
function report(version: string, world: World, dt: number): string {
	const first = world.positionOf(0);
	const position =
		first === null ? "—" : first.map((n) => n.toFixed(2)).join(", ");
	const fps = dt > 0 ? (1 / dt).toFixed(0) : "—";
	const { width, height } = world.viewport;
	return [
		`krudd ${version}`,
		`${world.entityCount} entities`,
		`frame ${world.frameCount}`,
		`${world.elapsed.toFixed(1)}s`,
		`${fps} fps`,
		`${width}x${height}`,
		`entity 0 at (${position})`,
	].join("  ·  ");
}

main().catch((error: unknown) => {
	// Loudly, and on the page: a boot failure that only reaches the console is
	// indistinguishable from a page that rendered nothing on purpose.
	const message = error instanceof Error ? error.message : String(error);
	const status = document.getElementById(STATUS_ID);
	if (status !== null) {
		status.textContent = `krudd failed to start — ${message}`;
		status.classList.add("failed");
	}
	console.error(error);
});
