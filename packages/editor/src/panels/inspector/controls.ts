// SPDX-License-Identifier: GPL-2.0-or-later
//
// Which control a parameter gets, decided from its declaration alone.
//
// ## The rule this file exists to enforce
//
// #951: **adding a parameter with an `(edit ...)` clause must produce a working
// control with zero editor code.** The failure mode it names is a `switch` on
// parameter name with a bespoke form per asset type — faster for the first
// three assets, and the reason feature number ten costs a week instead of an
// afternoon.
//
// So this is a pure function of `(edit, type, components)` and nothing else. It
// never sees a name, an asset path or a slot, which means it *cannot* special-
// case one — the constraint is enforced by what the function is given rather
// than by a reviewer noticing. A new parameter in a `.scm` file reaches a
// control by declaring a hint the engine already parses; no code here changes.
//
// ## Why "none" still gets a control
//
// A field with no `(edit ...)` clause is not undecorated by accident — most
// authored params have no hint at all. Rendering nothing would make those
// invisible, so `none` falls through to a plain number entry, which is the
// honest presentation of "a number with no declared bounds". #951 requires
// exactly this and calls it the fallback.

import type { ParamField } from "@kruddage/engine/bridge";

/**
 * The control kinds. One per shape a reader interacts with, not one per type:
 * `vec2` and `vec3` are both "several numbers side by side", and splitting them
 * would be two implementations of one behaviour.
 */
export type ControlKind = "slider" | "color" | "number" | "vector";

export interface ControlSpec {
	kind: ControlKind;
	/** How many entries the control edits. Always >= 1. */
	components: number;
	/** Present for "slider" only — the authored bounds. */
	min?: number;
	max?: number;
	/**
	 * Whether the value must stay integral. Carried separately from the
	 * control kind because an int can be any of them: `(edit range)` on an int
	 * is still a slider, it just steps by one.
	 */
	integral: boolean;
}

/**
 * The control a field's declaration asks for.
 *
 * Ordering matters and is deliberate: the authored hint is consulted *before*
 * the type, so a `vec3` tagged `(edit color)` becomes a colour well rather than
 * three number boxes. A file that checked the type first would silently ignore
 * every hint on a multi-component field.
 */
export function controlFor(field: ParamField): ControlSpec {
	const components = clampComponents(field.components);
	const integral = field.type === "int";

	if (field.edit === "color") {
		/*
		 * A colour is three or four components by construction. A hint of
		 * `color` on a scalar is a declaration mistake, and it falls through
		 * to a number rather than rendering a one-channel colour well that
		 * cannot mean anything.
		 */
		if (components >= 3) return { kind: "color", components, integral };
		return { kind: "number", components, integral };
	}

	if (field.edit === "range" && isUsableRange(field.min, field.max)) {
		return { kind: "slider", components, min: field.min, max: field.max, integral };
	}

	return components > 1
		? { kind: "vector", components, integral }
		: { kind: "number", components, integral };
}

/**
 * Whether a declared range is one a slider can be built from.
 *
 * A range of zero width, or one whose bounds arrived non-finite, would produce
 * a slider that cannot move. Falling back to a number entry keeps the parameter
 * editable, which is better than a control that is present and inert — and the
 * declaration is the thing at fault, not the reader.
 */
function isUsableRange(min: number, max: number): boolean {
	return Number.isFinite(min) && Number.isFinite(max) && max > min;
}

function clampComponents(components: number): number {
	if (!Number.isFinite(components) || components < 1) return 1;
	return Math.min(4, Math.floor(components));
}

/**
 * Constrain a value the way its control implies.
 *
 * Applied on the way *out* to the engine rather than on the way in: a value the
 * engine already holds is shown as it is, even if it sits outside the declared
 * range, because clamping the display would hide that a script or an older
 * project put it there. What the reader types is what gets constrained.
 */
export function constrain(spec: ControlSpec, value: number): number {
	if (!Number.isFinite(value)) return 0;
	let next = value;
	if (spec.kind === "slider") {
		next = Math.min(Math.max(next, spec.min ?? next), spec.max ?? next);
	}
	return spec.integral ? Math.round(next) : next;
}

/**
 * A field's value as a CSS colour, for the swatch.
 *
 * Components are 0..1 floats, which is what every shader and generator in the
 * tree works in. Alpha is dropped when present: the well shows the colour, and
 * a fourth component is edited as a number beside it rather than hidden inside
 * a picker that would quietly round it.
 */
export function toHex(value: readonly number[]): string {
	const byte = (n: number): string => {
		const clamped = Math.min(255, Math.max(0, Math.round((n ?? 0) * 255)));
		return clamped.toString(16).padStart(2, "0");
	};
	return `#${byte(value[0] ?? 0)}${byte(value[1] ?? 0)}${byte(value[2] ?? 0)}`;
}

/** The inverse of toHex, for what a colour input hands back. */
export function fromHex(hex: string): [number, number, number] | null {
	const match = /^#?([0-9a-f]{6})$/i.exec(hex.trim());
	if (!match) return null;
	const n = Number.parseInt(match[1] ?? "0", 16);
	return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
}
