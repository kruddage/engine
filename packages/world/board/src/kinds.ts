// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The node kinds the triangles project is made of.
 *
 * Every one of them is a pure function of (columns, params) — that is the
 * rule that keeps a later compile-to-TypeScript step a transform rather than
 * a redesign, and it is why a kind declares its ports and params rather than
 * reaching for the world it happens to be running in.
 *
 * The port types say the other rule out loud: what moves between nodes is
 * `column<vec3>`, a whole column, never one entity's `vec3`. `integrate`
 * takes the position and velocity columns and walks them once per frame.
 * `recycle` already exists in exactly this shape, hand-written, in
 * `packages/shell/web/src/index.ts` — it is a node nobody had drawn yet.
 *
 * A node never hardcodes which column it walks — it resolves a name from a
 * `column-name` param, once per phase, and asks the world for that column.
 * `position` and `velocity` are only the defaults; they are what makes the
 * triangles project keep running unchanged once the board declares them
 * itself. See `document.ts`'s `BoardColumn` for why the declaration lives on
 * the board rather than on whichever node happens to output the column.
 *
 * The defaults here are the constants the engine runs today, not
 * placeholders: changing `radius` here is changing where the triangles
 * spawn.
 */

import type { NodeKind, Registry } from "./document";

/** A whole column of positions or velocities, three floats per slot. */
const COLUMN: "column<vec3>" = "column<vec3>";

/** The param type a column-name param declares itself with. */
const COLUMN_NAME: "column-name" = "column-name";

/**
 * The kinds, by name.
 *
 * Names are lower-case and hyphenated; the title a node box shows is the
 * kind's, so the two never have to be kept in step by hand.
 */
export const KINDS: Registry = {
	start: {
		title: "Start",
		entry: "start",
		inputs: [],
		outputs: [],
		params: [],
	},

	"spawn-ring": {
		title: "Spawn Ring",
		inputs: [],
		outputs: [
			{ name: "position", type: COLUMN },
			{ name: "velocity", type: COLUMN },
		],
		params: [
			{ name: "count", type: "u32", default: 8, min: 0, max: 65536 },
			{ name: "radius", type: "f32", default: 0.4, min: 0 },
			{ name: "speed", type: "f32", default: 0.25, min: 0 },
			{ name: "position", type: COLUMN_NAME, default: "position" },
			{ name: "velocity", type: COLUMN_NAME, default: "velocity" },
		],
		run: (c) => {
			const count = c.number("count");
			const radius = c.number("radius");
			const speed = c.number("speed");
			// Slots first. Allocating is the one per-entity crossing left, and
			// it happens once when the board opens rather than once a frame, so
			// the rule the columns exist for is intact. A board that spawns
			// during play wants a batched allocate on the boundary; nothing does
			// yet.
			for (let slot = c.world.slotCount; slot < count; slot++) {
				c.world.spawn(0, 0, 0);
			}
			// Fetched after the spawns, never before: a spawn can move the
			// column, and a view taken first would be writing into memory that
			// is no longer it. Which column is this node's own business only in
			// name — `validate` has already checked the board declares it at a
			// matching kind.
			const position = c.world.column(c.text("position")) as Float32Array;
			const velocity = c.world.column(c.text("velocity")) as Float32Array;
			for (let i = 0; i < count; i++) {
				const angle = (i / count) * Math.PI * 2;
				position[i * 3] = Math.cos(angle) * radius;
				position[i * 3 + 1] = Math.sin(angle) * radius;
				position[i * 3 + 2] = 0;
				velocity[i * 3] = Math.cos(angle) * speed;
				velocity[i * 3 + 1] = Math.sin(angle) * speed;
				velocity[i * 3 + 2] = 0;
			}
		},
	},

	step: {
		title: "Step",
		entry: "step",
		inputs: [],
		outputs: [],
		params: [],
	},

	integrate: {
		title: "Integrate",
		inputs: [
			{ name: "position", type: COLUMN },
			{ name: "velocity", type: COLUMN },
		],
		outputs: [{ name: "position", type: COLUMN }],
		params: [
			{ name: "position", type: COLUMN_NAME, default: "position" },
			{ name: "velocity", type: COLUMN_NAME, default: "velocity" },
		],
		run: (c) => {
			const position = c.world.column(c.text("position")) as Float32Array;
			const velocity = c.world.column(c.text("velocity")) as Float32Array;
			// The whole column in one walk, tombstones included. A tombstoned
			// slot has a zero velocity and so integrates to itself, which is
			// cheaper than asking the engine which slots are live — and asking
			// per slot is the crossing this is all here to avoid.
			for (let i = 0; i < position.length; i += 3) {
				position[i] = (position[i] as number) + (velocity[i] as number) * c.dt;
				position[i + 1] =
					(position[i + 1] as number) + (velocity[i + 1] as number) * c.dt;
				position[i + 2] =
					(position[i + 2] as number) + (velocity[i + 2] as number) * c.dt;
			}
		},
	},

	recycle: {
		title: "Recycle",
		inputs: [{ name: "position", type: COLUMN }],
		outputs: [{ name: "position", type: COLUMN }],
		params: [
			// Comfortably outside the camera's box, so a recycle happens off
			// screen rather than as a visible jump.
			{ name: "limit", type: "f32", default: 3.2, min: 0 },
			{ name: "radius", type: "f32", default: 0.4, min: 0 },
			{ name: "position", type: COLUMN_NAME, default: "position" },
		],
		run: (c) => {
			const limit = c.number("limit");
			const radius = c.number("radius");
			const position = c.world.column(c.text("position")) as Float32Array;
			for (let i = 0; i < position.length; i += 3) {
				const x = position[i] as number;
				const y = position[i + 1] as number;
				const distance = Math.hypot(x, y);
				if (distance <= limit) {
					continue;
				}
				// Back onto the circle along the same ray, so an entity sets off
				// again in the direction it left by.
				const scale = radius / distance;
				position[i] = x * scale;
				position[i + 1] = y * scale;
			}
		},
	},

	paint: {
		title: "Paint",
		entry: "paint",
		inputs: [],
		outputs: [],
		params: [],
	},

	"draw-entities": {
		title: "Draw Entities",
		inputs: [{ name: "position", type: COLUMN }],
		outputs: [],
		params: [
			// One mesh so far, so `mesh` reports rather than decides. `scale`
			// decides: it reaches the transform of every draw, because a control
			// that looks live and changes nothing teaches the wrong thing about
			// what a board is.
			{ name: "mesh", type: "mesh", default: "triangle" },
			{ name: "scale", type: "f32", default: 0.35, min: 0 },
			// Named so `validate` can check it against the board's declared
			// columns like every other column-name param — but `render()` draws
			// from the engine's own position column regardless of what this
			// resolves to. Making the renderer take a column by name is a
			// boundary change nothing in this PR needs; until then this param
			// documents the dependency rather than redirecting it.
			{ name: "position", type: COLUMN_NAME, default: "position" },
		],
		run: (c) => {
			// Two crossings for the whole frame, whatever the draw count. The
			// draw list is built inside the engine from the position column —
			// the same column every node above has been walking.
			c.world.setScale(c.number("scale"));
			c.world.render();
		},
	},

	// One level down: the frame itself. Viewable rather than authorable —
	// these describe what the renderer already does, so that opening
	// `Draw Entities` shows clear → camera → draw → present instead of a
	// dead end. Authoring arbitrary render pipelines is a different problem
	// with a different audience.

	"begin-frame": {
		title: "Begin Frame",
		entry: "start",
		inputs: [],
		outputs: [{ name: "frame", type: "frame" }],
		params: [{ name: "surface", type: "surface", default: "canvas" }],
	},

	clear: {
		title: "Clear",
		inputs: [{ name: "frame", type: "frame" }],
		outputs: [{ name: "frame", type: "frame" }],
		// Black, which is what `Frame::new` clears to. The first thing the eye
		// sees is a setting rather than a constant.
		params: [{ name: "colour", type: "color", default: "#000000" }],
	},

	camera: {
		title: "Camera",
		inputs: [],
		outputs: [{ name: "viewProjection", type: "mat4" }],
		// Half the height of the visible world, in world units — VIEW_EXTENT in
		// crates/shell/web. The box is widened by the canvas aspect rather than
		// squashed, so a wider window shows more world.
		params: [{ name: "extent", type: "f32", default: 2, min: 0 }],
	},

	draw: {
		title: "Draw",
		inputs: [
			{ name: "position", type: COLUMN },
			{ name: "viewProjection", type: "mat4" },
		],
		outputs: [],
		params: [
			{ name: "pipeline", type: "mesh", default: "triangle" },
			{ name: "vertices", type: "u32", default: 3, min: 1 },
		],
	},

	present: {
		title: "Present",
		inputs: [{ name: "frame", type: "frame" }],
		outputs: [],
		params: [{ name: "to", type: "surface", default: "surface" }],
	},
};

/**
 * One kind by name, or `undefined` if the registry does not hold it.
 *
 * The `hasOwn` is the whole function. A document names its kinds as strings,
 * and a document is untrusted input: indexing a plain object with
 * `"constructor"` or `"toString"` walks the prototype chain and hands back
 * something that is not a kind but is not `undefined` either. Every lookup
 * keyed by a name a document chose goes through here or through
 * `Object.hasOwn` for that reason.
 */
export function kindOf(kinds: Registry, name: string): NodeKind | undefined {
	return Object.hasOwn(kinds, name) ? kinds[name] : undefined;
}
