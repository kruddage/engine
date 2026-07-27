// SPDX-License-Identifier: GPL-2.0-or-later

import type { ColumnKind } from "./document";

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
	 * Creates a named column if it does not already exist, sized to the
	 * current slot count. See `@krudd/boundary`'s `ensureColumn`.
	 *
	 * The `Runner` calls this once per column a board declares, before the
	 * Start lane runs — a node's `run` never has to, which is what keeps a
	 * node a function of the columns it is handed rather than of whether they
	 * happen to exist yet.
	 */
	ensureColumn(name: string, kind: ColumnKind): void;
	/**
	 * A named column, over the engine's own memory: three floats per slot for
	 * a `vec3` column, one for `f32`, one `u32` count for `u32`.
	 *
	 * Fetched where it is used and never stored, because a `spawn` can move it
	 * and growing wasm memory detaches it silently. See `docs/boundary.md`.
	 */
	column(name: string): Float32Array | Uint32Array;
	/** How many slots the columns cover, live and tombstoned alike. */
	readonly slotCount: number;
	/**
	 * World-space `x` for a normalised screen-space `x` in `[0, 1]`, 0 at the
	 * viewport's left edge.
	 *
	 * The unprojection a picking node needs and must never redo itself: it
	 * reads the same camera matrix the renderer draws with, in
	 * `Engine::world_x_from_screen` (`crates/shell/web/src/lib.rs`), so a pick
	 * can never disagree with the draw at some aspect ratio. `place-mark`
	 * calls this rather than owning a second copy of the camera arithmetic.
	 */
	worldXFromScreen(x: number): number;
	/**
	 * World-space `y` for a normalised screen-space `y` in `[0, 1]`, 0 at the
	 * viewport's top edge. See `worldXFromScreen` — same reasoning, and the
	 * one place the two differ is the sign flip screen-down/world-up needs.
	 */
	worldYFromScreen(y: number): number;
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
 * One pointer sample, however it reached the graph.
 *
 * Viewport-relative and axis-independent: `x` is how far across the viewport
 * the pointer sits, 0 at the left edge and 1 at the right; `y` is how far
 * down, 0 at the top and 1 at the bottom. Each axis is divided by its own
 * extent rather than by one shared scale, so a resize does not move where a
 * tap lands and a non-square viewport does not skew it — the centre of the
 * pane is always `(0.5, 0.5)`, phone or not.
 *
 * `pressed` is edge-triggered: 1 for exactly the one frame a press began on,
 * 0 every other frame — held, released, or never touched. The pre-rewrite
 * engine debounced on a `g_last_sel` comparison for exactly this reason, and
 * without the edge a held press would place on every frame it stays down.
 * Where the edge is found is `packages/shell/web/src/pointer.ts`'s job; this
 * type only says what a node is handed once it has been.
 */
export interface PointerFrame {
	/** Left-to-right across the viewport: 0 at the left edge, 1 at the right. */
	readonly x: number;
	/** Top-to-bottom down the viewport: 0 at the top, 1 at the bottom. */
	readonly y: number;
	/** 1 for the one frame a press began on; 0 otherwise. Carries `u32` on the wire. */
	readonly pressed: number;
}

/**
 * Nothing pressed, and nowhere in particular.
 *
 * What a board reads before any pointer event has reached it — including
 * every board that never asks, like the triangles demo, which takes no input
 * and must not start taking any.
 */
export const NO_POINTER: PointerFrame = { x: 0, y: 0, pressed: 0 };

/**
 * What one node is handed when it runs.
 *
 * Columns, params and the pointer, and nothing else — a node is a pure
 * function of what is handed to it here. That is what keeps a later
 * compile-to-TypeScript step a transform rather than a redesign: a node that
 * reached for anything wider — the DOM included — would have to have that
 * thing invented for it in the generated code as well.
 */
export interface RunContext {
	/** The world whose columns this node walks. */
	readonly world: WorldView;
	/** Seconds since the last frame, already clamped. Zero in the start lane. */
	readonly dt: number;
	/**
	 * This frame's pointer sample.
	 *
	 * [`NO_POINTER`] outside a game that reads it — the demo included. A node
	 * that wants pointer input reads this directly, the same way `integrate`
	 * reads `world.positions()`, rather than through a wire this interpreter
	 * evaluates.
	 */
	readonly pointer: PointerFrame;
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
