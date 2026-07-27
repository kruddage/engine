// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * A world that is nothing but named columns, for the suites that run a board.
 *
 * The point of running the interpreter without wasm is that the tests can say
 * what the *numbers* are. `render-test` proves the document drives the real
 * engine — pixel for pixel, which is the acceptance criterion that matters —
 * but a screenshot cannot tell you that entity three is at
 * x = 0.4 + 0.25 × 1.5, and that is the assertion that catches an interpreter
 * which is wrong by a frame or by a factor of dt.
 *
 * This satisfies `WorldView` structurally, exactly as `@krudd/boundary`'s
 * `World` does. It grows every column on `spawn`, together, the way the real
 * engine's `grow_columns_to_fit` does — and hands back fresh arrays
 * afterwards, which is not a convenience — it is the boundary's own rule, that
 * a `spawn` can move a column and a view taken beforehand is addressing memory
 * that is no longer it. A node that cached its view across a spawn fails here
 * rather than in a browser.
 *
 * Shared rather than written out in each suite: two fake worlds that drift
 * apart are two suites that disagree about what the engine promises, and the
 * one that is wrong is the one nobody notices.
 */

import type { ColumnKind, WorldView } from "../src/index";

/** The position column's name, exactly as `@krudd/boundary`'s does. */
const POSITION = "position";

/** The velocity column's name, exactly as `@krudd/boundary`'s does. */
const VELOCITY = "velocity";

/** How many `f32`s or `u32`s one slot of a column of this kind occupies. */
function componentsOf(kind: ColumnKind): number {
	return kind === "vec3" ? 3 : 1;
}

/** A column of `length` elements, zero-filled, at the right element width. */
function makeColumn(
	kind: ColumnKind,
	length: number,
): Float32Array<ArrayBuffer> | Uint32Array<ArrayBuffer> {
	return kind === "u32" ? new Uint32Array(length) : new Float32Array(length);
}

/** A world that is named columns and a draw counter. */
export class TestWorld implements WorldView {
	readonly #kinds = new Map<string, ColumnKind>();
	readonly #data = new Map<
		string,
		Float32Array<ArrayBuffer> | Uint32Array<ArrayBuffer>
	>();
	#slots = 0;
	/** How many times the paint lane reached the renderer. */
	draws = 0;
	/** How many frames were presented with nothing in them. */
	blanks = 0;
	/** The scale the paint lane last asked for. */
	scale = 0;
	/**
	 * The camera half-extents `worldXFromScreen`/`worldYFromScreen` unproject
	 * through: world units from the centre to each edge.
	 *
	 * The real engine derives these from `Engine::view_projection`, which
	 * itself derives them from the canvas's aspect ratio — there is no canvas
	 * here, so this is a plain settable field rather than something computed.
	 * That is what "configurable enough to test picking without wasm" means: a
	 * suite that wants to prove a pick lands on the cell it should can set up
	 * a known viewport without booting wasm or touching a canvas at all.
	 * Defaults to a square viewport at `VIEW_EXTENT = 2`
	 * (`crates/shell/web/src/lib.rs`), which is what `Engine::new(n, n)` gives
	 * for any square canvas — and, not coincidentally, `pick-grid`'s and
	 * `spawn-grid`'s own default `size`.
	 */
	camera = { halfWidth: 2, halfHeight: 2 };

	constructor() {
		// The real engine creates these two at boot, through the same path
		// `ensureColumn` takes, and `spawn` writes into them by these literal
		// names regardless of what a board declares. Mirrored here so `spawn`
		// always has somewhere to write, exactly as it does in the browser.
		this.ensureColumn(POSITION, "vec3");
		this.ensureColumn(VELOCITY, "vec3");
	}

	get slotCount(): number {
		return this.#slots;
	}

	spawn(x: number, y: number, z: number): number {
		const slot = this.#slots;
		this.#slots += 1;
		// Every column grows together, whatever a board declared — new arrays,
		// not resized ones, because the real columns move when they grow and a
		// node holding a stale view must fail here too.
		for (const [name, kind] of this.#kinds) {
			const grown = makeColumn(kind, this.#slots * componentsOf(kind));
			const existing = this.#data.get(name);
			if (existing !== undefined) {
				grown.set(existing as ArrayLike<number>);
			}
			this.#data.set(name, grown);
		}
		const position = this.#data.get(POSITION) as Float32Array;
		position.set([x, y, z], slot * 3);
		return slot;
	}

	ensureColumn(name: string, kind: ColumnKind): void {
		const existing = this.#kinds.get(name);
		if (existing !== undefined) {
			if (existing !== kind) {
				throw new Error(
					`column "${name}" already exists as ${existing}; refusing to ` +
						`re-create it as ${kind}`,
				);
			}
			return;
		}
		this.#kinds.set(name, kind);
		this.#data.set(name, makeColumn(kind, this.#slots * componentsOf(kind)));
	}

	column(name: string): Float32Array | Uint32Array {
		const held = this.#data.get(name);
		if (held === undefined) {
			throw new Error(
				`no column named "${name}" — call ensureColumn(name, kind) first`,
			);
		}
		return held;
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

	worldXFromScreen(x: number): number {
		// The exact per-axis formula `Engine::world_x_from_screen` uses: screen
		// `[0, 1]` maps to clip `[-1, 1]`, scaled by the camera's half-width.
		return (x * 2 - 1) * this.camera.halfWidth;
	}

	worldYFromScreen(y: number): number {
		// Screen `y` runs down; world `y` runs up, so `y = 0` (the top of the
		// viewport) has to land at `+halfHeight`, not `-halfHeight` — the same
		// sign flip `Engine::world_y_from_screen`'s own docs call out as the
		// one place this has to differ from the x formula.
		return (1 - y * 2) * this.camera.halfHeight;
	}

	/** The position column, for suites that already assume it exists. */
	positions(): Float32Array {
		return this.column(POSITION) as Float32Array;
	}

	/** The velocity column, for suites that already assume it exists. */
	velocities(): Float32Array {
		return this.column(VELOCITY) as Float32Array;
	}
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
