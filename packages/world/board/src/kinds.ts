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
 * The defaults here are the constants the engine runs today, not
 * placeholders: changing `radius` here is changing where the triangles
 * spawn.
 */

import type { NodeKind, Registry } from "./document";

/** A whole column of positions or velocities, three floats per slot. */
const COLUMN: "column<vec3>" = "column<vec3>";

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
		],
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
		params: [],
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
		],
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
			{ name: "mesh", type: "mesh", default: "triangle" },
			{ name: "scale", type: "f32", default: 0.35, min: 0 },
		],
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
