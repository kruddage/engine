// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * What a node needs from the world it runs against, and what it is handed.
 *
 * A structural interface rather than an import of `@krudd/boundary`'s
 * `World`, and that is deliberate on two counts. It keeps `@krudd/board`
 * importing nothing, so a package describing the scene data model does not
 * quietly acquire the wasm loader as a dependency. And it means the
 * interpreter can be held against a plain in-memory world under `node:test`,
 * with no wasm build in the loop — the proof that it drives the *real* engine
 * is `cargo xtask render-test`, pixel for pixel, which is the acceptance that
 * matters anyway.
 *
 * `World` satisfies this already. Nothing has to be written to make it fit.
 */

/** The part of a world a node may touch. */
export interface WorldView {
	/** Allocates one entity and returns its slot index. */
	spawn(x: number, y: number, z: number): number;
	/**
	 * The position column: three floats per slot, over the engine's own
	 * memory.
	 *
	 * Fetched where it is used and never stored, because a `spawn` can move it
	 * and growing wasm memory detaches it silently. See `docs/boundary.md`.
	 */
	positions(): Float32Array;
	/** The velocity column, laid out and invalidated exactly like positions. */
	velocities(): Float32Array;
	/** How many slots the columns cover, live and tombstoned alike. */
	readonly slotCount: number;
	/** Draws the world. One crossing for the whole frame. */
	render(): void;
	/**
	 * Presents a frame with nothing in it.
	 *
	 * What a board with nothing in its paint lane leaves on the screen. The
	 * alternative is the last frame it drew, which would make a cut wire look
	 * like a frozen game rather than like a blank one.
	 */
	presentCleared(): void;
	/** Sets how large one entity draws, in world units. */
	setScale(scale: number): void;
}

/**
 * What one node is handed when it runs.
 *
 * Columns and params, and nothing else — a node is a pure function of the
 * two. That is what keeps a later compile-to-TypeScript step a transform
 * rather than a redesign: a node that reached for anything wider would have
 * to have that thing invented for it in the generated code as well.
 */
export interface RunContext {
	/** The world whose columns this node walks. */
	readonly world: WorldView;
	/** Seconds since the last frame, already clamped. Zero in the start lane. */
	readonly dt: number;
	/**
	 * A numeric param, resolved from the node or its kind's default.
	 *
	 * Throws rather than returning a fallback. Validation has already refused
	 * a param that is missing or of the wrong type, so reaching this is a bug
	 * in the interpreter rather than in the document, and a silent zero would
	 * present as an entity that did not move.
	 */
	number(name: string): number;
	/** A named param — a mesh, a colour, a surface. Throws for the same reason. */
	text(name: string): string;
}

/** What a node kind does when the frame comes round. */
export type Run = (context: RunContext) => void;
