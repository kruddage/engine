// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * A world that is nothing but two columns, for the suites that run a board.
 *
 * The point of running the interpreter without wasm is that the tests can say
 * what the *numbers* are. `render-test` proves the document drives the real
 * engine — pixel for pixel, which is the acceptance criterion that matters —
 * but a screenshot cannot tell you that entity three is at
 * x = 0.4 + 0.25 × 1.5, and that is the assertion that catches an interpreter
 * which is wrong by a frame or by a factor of dt.
 *
 * This satisfies `WorldView` structurally, exactly as `@krudd/boundary`'s
 * `World` does. It grows its columns on `spawn` and hands back fresh arrays
 * afterwards, which is not a convenience — it is the boundary's own rule, that
 * a `spawn` can move a column and a view taken beforehand is addressing memory
 * that is no longer it. A node that cached its view across a spawn fails here
 * rather than in a browser.
 *
 * Shared rather than written out in each suite: two fake worlds that drift
 * apart are two suites that disagree about what the engine promises, and the
 * one that is wrong is the one nobody notices.
 */

import type { WorldView } from "../src/index";

/** A world that is two columns and a draw counter. */
export class TestWorld implements WorldView {
	#positions: Float32Array<ArrayBuffer> = new Float32Array(0);
	#velocities: Float32Array<ArrayBuffer> = new Float32Array(0);
	/** How many times the paint lane reached the renderer. */
	draws = 0;
	/** How many frames were presented with nothing in them. */
	blanks = 0;
	/** The scale the paint lane last asked for. */
	scale = 0;

	get slotCount(): number {
		return this.#positions.length / 3;
	}

	spawn(x: number, y: number, z: number): number {
		const slot = this.slotCount;
		// New arrays, not resized ones — the real columns move when they grow,
		// and a node holding a stale view must fail here too.
		this.#positions = grown(this.#positions);
		this.#velocities = grown(this.#velocities);
		this.#positions.set([x, y, z], slot * 3);
		return slot;
	}

	positions(): Float32Array {
		return this.#positions;
	}

	velocities(): Float32Array {
		return this.#velocities;
	}

	render(): void {
		this.draws += 1;
	}

	presentCleared(): void {
		this.blanks += 1;
	}

	setScale(scale: number): void {
		this.scale = scale;
	}
}

/** One more slot's worth of column, carrying what was already there. */
function grown(column: Float32Array<ArrayBuffer>): Float32Array<ArrayBuffer> {
	const next = new Float32Array(column.length + 3);
	next.set(column);
	return next;
}

/** One entity's position. */
export function at(world: TestWorld, slot: number): [number, number, number] {
	const p = world.positions();
	return [
		p[slot * 3] as number,
		p[slot * 3 + 1] as number,
		p[slot * 3 + 2] as number,
	];
}
