// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Booting the wasm module under Node, for the harnesses that assert on the
 * boundary itself.
 *
 * The engine's own boot path is `boot()` in `../src/index`, which fetches the
 * wasm over HTTP because that is what a page does. Node has no `fetch` for
 * `file://`, so the harnesses hand wasm-bindgen the bytes instead and build
 * the `World` themselves. That is the only difference: the module, the glue
 * and the wrapper under test are the ones the browser gets, built by the same
 * `cargo xtask build-web`.
 *
 * Everything here is run by `cargo xtask`, never by `node` directly — the
 * glue this imports does not exist until the wasm is built.
 */

import { readFileSync } from "node:fs";
import init, { Engine, type InitOutput } from "../generated/krudd_web.js";
import { World } from "../src/index";

/**
 * Where the wasm is, when `cargo xtask` has not said.
 *
 * Relative to the workspace root, which is where xtask runs node from.
 */
const DEFAULT_WASM = "packages/base/boundary/generated/krudd_web_bg.wasm";

/** A module instantiated under Node. */
export interface Loaded {
	/** The wrapper under test. */
	readonly world: World;
	/**
	 * The generated binding, unwrapped.
	 *
	 * The harnesses need it because they assert on things the wrapper exists
	 * to hide — the raw pointer, and the per-call path the contract forbids.
	 */
	readonly engine: Engine;
	/** The module's linear memory. */
	readonly memory: WebAssembly.Memory;
}

/**
 * Instantiates the module and returns a world over it.
 *
 * wasm-bindgen's `init` caches per process, so every call after the first
 * shares one instance and one linear memory. Each call still gets its own
 * `Engine` with its own columns, which is the isolation a harness needs;
 * anything asserting on *memory growth* has to force it rather than assume a
 * fresh 1 MiB heap.
 */
export async function load(width = 1, height = 1): Promise<Loaded> {
	const path = process.env.KRUDD_WASM ?? DEFAULT_WASM;
	let bytes: Uint8Array;
	try {
		bytes = readFileSync(path);
	} catch (cause) {
		throw new Error(
			`${path}: no wasm to load — run \`cargo xtask build-web\` first, ` +
				"or set KRUDD_WASM to the module's path",
			{ cause },
		);
	}
	const output: InitOutput = await init({ module_or_path: bytes });
	const engine = new Engine(width, height);
	return {
		world: new World(engine, output.memory),
		engine,
		memory: output.memory,
	};
}

/**
 * Spawns `count` entities on a circle, each drifting outward.
 *
 * Shared by the tests and the benchmark so both measure and assert against
 * the same world rather than two worlds that happen to look alike.
 */
export function populate(world: World, count: number): void {
	for (let i = 0; i < count; i++) {
		const angle = (i / count) * Math.PI * 2;
		const slot = world.spawn(Math.cos(angle), Math.sin(angle), 0);
		world.setVelocity(slot, Math.cos(angle) * 0.1, Math.sin(angle) * 0.1, 0);
	}
}
