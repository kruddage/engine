// SPDX-License-Identifier: GPL-2.0-or-later
//
// The outliner's model, driven from plain data.
//
// Everything here is a question the panel asks that would be expensive to ask
// through a mounted, virtualized tree and cheap to get wrong: which rows a
// range covers, which of a multi-row drag actually have to move, what a search
// matches. The component suite next door mounts the thing; this one exercises
// the arithmetic behind it.

import { describe, expect, it } from "vitest";

import type { SceneTree, TreeNode } from "@kruddage/engine/bridge";

import {
	buildTree,
	displayName,
	dragRoots,
	entityId,
	flatten,
	isAncestor,
	matches,
	rowId,
	subtree,
} from "../src/panels/outliner/tree.js";

function node(id: number, parent: number, name: string | null = null): TreeNode {
	return { id, parent, name, mask: 0, render: 0, material: 0, script: 0, flags: 0 };
}

function scene(entities: TreeNode[]): SceneTree {
	return { paused: false, entities };
}

/*
 * root(0)
 *   crate(1)
 *     lid(3)
 *   lamp(2)
 */
const NESTED = scene([
	node(0, -1, "root"),
	node(1, 0, "crate"),
	node(2, 0, "lamp"),
	node(3, 1, "lid"),
]);

describe("building the tree", () => {
	it("nests children under their parent", () => {
		const [root] = buildTree(NESTED);
		expect(root?.name).toBe("root");
		expect(root?.children?.map((r) => r.name)).toEqual(["crate", "lamp"]);
		expect(root?.children?.[0]?.children?.map((r) => r.name)).toEqual(["lid"]);
	});

	it("leaves a leaf without a children array at all", () => {
		/* An empty array draws a twisty on something that cannot open. */
		const lid = flatten(buildTree(NESTED)).find((r) => r.entity === 3);
		expect(lid?.children).toBeUndefined();
	});

	it("does not need parents to come before children", () => {
		/*
		 * The property #950 gave up in the world so ids could stay stable
		 * across a reparent: the engine no longer promises this list is
		 * topological, and a builder that assumed it would silently drop rows.
		 */
		const backwards = scene([node(3, 1, "lid"), node(1, 0, "crate"), node(0, -1, "root")]);
		const [root] = buildTree(backwards);
		expect(root?.name).toBe("root");
		expect(root?.children?.[0]?.children?.[0]?.name).toBe("lid");
	});

	it("places an entity whose parent is missing at the root", () => {
		/* Should not happen — destroy cascades — but a row a reader can see
		 * and delete beats one that is in the scene and not on screen. */
		const orphan = scene([node(5, 99, "stray")]);
		expect(buildTree(orphan).map((r) => r.entity)).toEqual([5]);
	});

	it("is empty when the engine has not answered", () => {
		expect(buildTree(null)).toEqual([]);
	});

	it("names an unnamed entity after its id rather than nothing", () => {
		expect(displayName(node(12, -1))).toBe("Entity 12");
		expect(displayName(node(12, -1, ""))).toBe("Entity 12");
		expect(displayName(node(12, -1, "crate"))).toBe("crate");
	});

	it("round-trips a row id", () => {
		expect(entityId(rowId(41))).toBe(41);
	});
});

describe("the rows a gesture actually applies to", () => {
	it("drops a row whose parent is being dragged too", () => {
		/*
		 * Reparenting a child that is already travelling with its parent would
		 * flatten the hierarchy the reader was trying to move intact.
		 */
		expect(dragRoots(buildTree(NESTED), [1, 3])).toEqual([1]);
	});

	it("drops a grandchild, not just a child", () => {
		expect(dragRoots(buildTree(NESTED), [0, 1, 2, 3])).toEqual([0]);
	});

	it("keeps siblings, which are independent", () => {
		expect(dragRoots(buildTree(NESTED), [1, 2])).toEqual([1, 2]);
	});

	it("collects a subtree parents first", () => {
		expect(subtree(buildTree(NESTED), 0)).toEqual([0, 1, 3, 2]);
		expect(subtree(buildTree(NESTED), 3)).toEqual([3]);
		expect(subtree(buildTree(NESTED), 99)).toEqual([]);
	});
});

describe("refusing a cycle before the drop", () => {
	it("sees a node as its own ancestor", () => {
		expect(isAncestor(buildTree(NESTED), 1, 1)).toBe(true);
	});

	it("sees a grandparent", () => {
		expect(isAncestor(buildTree(NESTED), 0, 3)).toBe(true);
	});

	it("does not see a descendant as an ancestor", () => {
		expect(isAncestor(buildTree(NESTED), 3, 0)).toBe(false);
	});

	it("does not see a sibling", () => {
		expect(isAncestor(buildTree(NESTED), 1, 2)).toBe(false);
	});
});

describe("the search", () => {
	const rows = flatten(buildTree(NESTED));
	const crate = rows.find((r) => r.entity === 1)!;

	it("matches a name case-insensitively, anywhere in it", () => {
		expect(matches(crate, "RAT")).toBe(true);
	});

	it("matches an id exactly, so an unnamed row is findable", () => {
		expect(matches(crate, "1")).toBe(true);
		expect(matches(crate, "11")).toBe(false);
	});

	it("matches everything on an empty or blank term", () => {
		expect(matches(crate, "")).toBe(true);
		expect(matches(crate, "   ")).toBe(true);
	});

	it("does not match an unrelated word", () => {
		expect(matches(crate, "lamp")).toBe(false);
	});
});
