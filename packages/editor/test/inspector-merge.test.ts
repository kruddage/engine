// SPDX-License-Identifier: GPL-2.0-or-later
//
// What several entities have in common, and what "in common" is allowed to
// mean. The panel suite drives this through the DOM; this drives it directly,
// because the rules worth getting wrong here are the ones that produce a form
// that looks right and writes to the wrong field.

import { describe, expect, it } from "vitest";

import type { EntityParams, ParamBlock, ParamField } from "@kruddage/engine/bridge";

import { mergeParams } from "../src/panels/inspector/merge.js";

function field(overrides: Partial<ParamField> = {}): ParamField {
	return {
		name: "width",
		type: "float",
		components: 1,
		edit: "none",
		min: 0,
		max: 0,
		value: [1],
		...overrides,
	};
}

function block(fields: ParamField[], overrides: Partial<ParamBlock> = {}): ParamBlock {
	return {
		slot: "mesh",
		asset: 42,
		path: "m",
		size: 4,
		overridden: true,
		truncated: false,
		fields,
		...overrides,
	};
}

function entity(id: number, blocks: ParamBlock[]): EntityParams {
	return { id, blocks };
}

describe("one entity", () => {
	it("passes everything through, addressed to itself", () => {
		const { blocks, pending } = mergeParams([
			entity(3, [block([field(), field({ name: "depth" })])]),
		]);

		expect(pending).toBe(0);
		expect(blocks).toHaveLength(1);
		expect(blocks[0]?.fields.map((f) => f.declaration.name)).toEqual([
			"width",
			"depth",
		]);
		expect(blocks[0]?.fields[1]?.targets).toEqual([{ id: 3, slot: 0, field: 1 }]);
		expect(blocks[0]?.fields[0]?.mixed).toBe(false);
		expect(blocks[0]?.unshared).toBe(0);
	});

	it("has nothing to show before the engine has answered", () => {
		expect(mergeParams([null])).toEqual({ blocks: [], pending: 1 });
	});
});

describe("what several entities share", () => {
	it("matches a parameter by its declaration, not by where it sits", () => {
		/*
		 * The rule the whole file exists for. Two meshes declaring the same two
		 * parameters in opposite orders must produce one row per parameter,
		 * each addressed to the index its own asset put it at.
		 */
		const { blocks } = mergeParams([
			entity(3, [block([field({ name: "width" }), field({ name: "depth" })])]),
			entity(4, [block([field({ name: "depth" }), field({ name: "width" })])]),
		]);

		expect(blocks[0]?.fields[0]?.declaration.name).toBe("width");
		expect(blocks[0]?.fields[0]?.targets).toEqual([
			{ id: 3, slot: 0, field: 0 },
			{ id: 4, slot: 0, field: 1 },
		]);
	});

	it("keeps the primary's order, whatever the rest declare", () => {
		const { blocks } = mergeParams([
			entity(3, [block([field({ name: "depth" }), field({ name: "width" })])]),
			entity(4, [block([field({ name: "width" }), field({ name: "depth" })])]),
		]);

		expect(blocks[0]?.fields.map((f) => f.declaration.name)).toEqual([
			"depth",
			"width",
		]);
	});

	it("does not share a name whose declaration differs", () => {
		/*
		 * Same name, same type, different authored bounds. One slider cannot be
		 * honest about both, and building one from the primary's declaration
		 * would clamp the other entity's value to a range its asset never
		 * asked for.
		 */
		const { blocks } = mergeParams([
			entity(3, [block([field({ edit: "range", min: 0, max: 1 })])]),
			entity(4, [block([field({ edit: "range", min: 0, max: 10 })])]),
		]);

		expect(blocks[0]?.fields).toEqual([]);
		expect(blocks[0]?.unshared).toBe(1);
	});

	it("does not share a name whose type or width differs", () => {
		const { blocks } = mergeParams([
			entity(3, [block([field({ type: "vec3", components: 3 })])]),
			entity(4, [block([field({ type: "float", components: 1 })])]),
		]);

		expect(blocks[0]?.fields).toEqual([]);
	});

	it("counts what it left out rather than dropping it silently", () => {
		const { blocks } = mergeParams([
			entity(3, [block([field(), field({ name: "wobbliness" })])]),
			entity(4, [block([field()])]),
		]);

		expect(blocks[0]?.fields).toHaveLength(1);
		expect(blocks[0]?.unshared).toBe(1);
	});

	it("drops a slot that is not on every one of them", () => {
		const { blocks } = mergeParams([
			entity(3, [
				block([field()]),
				block([field()], { slot: "script", asset: 9, path: "s" }),
			]),
			entity(4, [block([field()])]),
		]);

		expect(blocks.map((b) => b.slot)).toEqual(["mesh"]);
	});

	it("finds a shared slot wherever each entity happens to keep it", () => {
		/* An entity with no mesh puts its script block at index 0, and a write
		 * is addressed by that index. */
		const { blocks } = mergeParams([
			entity(3, [
				block([field()]),
				block([field()], { slot: "script", asset: 9, path: "s" }),
			]),
			entity(4, [block([field()], { slot: "script", asset: 9, path: "s" })]),
		]);

		expect(blocks.map((b) => b.slot)).toEqual(["script"]);
		expect(blocks[0]?.fields[0]?.targets).toEqual([
			{ id: 3, slot: 1, field: 0 },
			{ id: 4, slot: 0, field: 0 },
		]);
	});
});

describe("what several entities disagree about", () => {
	it("reports a differing value as mixed", () => {
		const { blocks } = mergeParams([
			entity(3, [block([field({ value: [1] })])]),
			entity(4, [block([field({ value: [2] })])]),
		]);

		expect(blocks[0]?.fields[0]?.mixed).toBe(true);
		/* The primary's value is still carried: a control that has to render
		 * something — a slider, a colour well — renders this. */
		expect(blocks[0]?.fields[0]?.value).toEqual([1]);
	});

	it("compares every component, not the first", () => {
		const vec = (value: number[]): ParamField =>
			field({ type: "vec3", components: 3, value });
		const { blocks } = mergeParams([
			entity(3, [block([vec([1, 2, 3])])]),
			entity(4, [block([vec([1, 2, 4])])]),
		]);

		expect(blocks[0]?.fields[0]?.mixed).toBe(true);
	});

	it("is not mixed when they agree", () => {
		const { blocks } = mergeParams([
			entity(3, [block([field({ value: [1] })])]),
			entity(4, [block([field({ value: [1] })])]),
		]);

		expect(blocks[0]?.fields[0]?.mixed).toBe(false);
	});

	it("names the asset when it is one and counts them when it is not", () => {
		const one = mergeParams([
			entity(3, [block([field()], { path: "m" })]),
			entity(4, [block([field()], { path: "m" })]),
		]);
		expect(one.blocks[0]?.source).toBe("m");

		const several = mergeParams([
			entity(3, [block([field()], { path: "m" })]),
			entity(4, [block([field()], { path: "other" })]),
		]);
		expect(several.blocks[0]?.source).toBe("2 assets");
	});

	it("is at defaults only when all of them are", () => {
		const { blocks } = mergeParams([
			entity(3, [block([field()], { overridden: false })]),
			entity(4, [block([field()], { overridden: true })]),
		]);

		expect(blocks[0]?.overridden).toBe(true);
	});

	it("carries a truncation any one of them reported", () => {
		const { blocks } = mergeParams([
			entity(3, [block([field()], { truncated: false })]),
			entity(4, [block([field()], { truncated: true })]),
		]);

		expect(blocks[0]?.truncated).toBe(true);
	});

	it("merges over what has arrived and counts what has not", () => {
		const { blocks, pending } = mergeParams([
			entity(3, [block([field()])]),
			null,
			entity(5, [block([field()])]),
		]);

		expect(pending).toBe(1);
		expect(blocks[0]?.fields[0]?.targets.map((t) => t.id)).toEqual([3, 5]);
	});
});
