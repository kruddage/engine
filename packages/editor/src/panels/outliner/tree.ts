// SPDX-License-Identifier: GPL-2.0-or-later
//
// The outliner's model: a flat scene turned into a tree, and nothing else.
//
// This file is pure. It takes the `scene.tree` the engine answered with and
// produces the shape `react-arborist` wants, plus the handful of questions the
// panel asks about that shape — where a search matched, which rows a range
// select covers, what order a multi-row drag should be applied in.
//
// Pure because every one of those is a place a bug hides silently. A range
// select that walks the wrong rows is invisible until someone deletes with it,
// and a test that has to mount a virtualized tree to catch that is a test
// nobody writes. Everything here is driven from plain data in
// test/outliner-tree.test.ts, and the component is left with the DOM.
//
// ## It holds no state, including the selection
//
// There is no "selected" field on a row. The selection lives in the engine
// (#950 made it a set there rather than adding one here), and the panel reads
// it back through `useSelection()` like the viewport does. What this file
// knows about selection is only how to turn a gesture — a shift-click, a
// ctrl-click — into the ids that gesture names.

import type { SceneTree, TreeNode } from "@kruddage/engine/bridge";

/**
 * A row, as react-arborist consumes it.
 *
 * `id` is a string because that is what the library keys by; it is the entity
 * id rendered in base 10 and nothing more. Every crossing back into command
 * territory goes through `entityId()` rather than a bare `Number(...)` at the
 * call site, so there is one place that can be wrong.
 */
export interface Row {
	id: string;
	entity: number;
	name: string;
	/** Absent rather than empty for a leaf — arborist draws a twisty on `[]`. */
	children?: Row[];
	node: TreeNode;
}

/** The id an entity's row carries. */
export function rowId(entity: number): string {
	return String(entity);
}

/** The entity a row id names. NaN never reaches a command — callers guard. */
export function entityId(row: string): number {
	return Number.parseInt(row, 10);
}

/**
 * What a row is called when the entity has no name.
 *
 * The id, spelled out, rather than a blank. An unnamed entity is ordinary —
 * `scene-spawn` does not require a name and a script that makes fifty of them
 * names none — and fifty blank rows are indistinguishable from each other and
 * from a broken panel.
 */
export function displayName(node: TreeNode): string {
	return node.name && node.name.length > 0 ? node.name : `Entity ${node.id}`;
}

/**
 * Build the nested tree.
 *
 * The engine answers with a flat list and a parent column, and it no longer
 * promises parent-before-child order — #950 relaxed that in the world so that
 * ids could stay stable across a reparent. So this indexes first and links
 * second rather than assuming it can build in one pass.
 *
 * An entity whose parent is not in the list is placed at the root rather than
 * dropped. That should not happen — the engine cascades a destroy — but a row
 * the reader can see and delete beats a row that is in the scene and not on
 * screen.
 */
export function buildTree(tree: SceneTree | null): Row[] {
	if (!tree) return [];

	const rows = new Map<number, Row>();
	for (const node of tree.entities) {
		rows.set(node.id, {
			id: rowId(node.id),
			entity: node.id,
			name: displayName(node),
			node,
		});
	}

	const roots: Row[] = [];
	for (const node of tree.entities) {
		const row = rows.get(node.id);
		if (!row) continue;
		const parent = node.parent >= 0 ? rows.get(node.parent) : undefined;
		if (!parent) {
			roots.push(row);
			continue;
		}
		(parent.children ??= []).push(row);
	}
	return roots;
}

/**
 * Every row in the order the tree shows them, ignoring what is collapsed.
 *
 * The full flatten rather than the visible rows, because every caller below
 * asks a question about the scene rather than about what is on screen: which
 * rows a subtree covers, whether one row is inside another. Range selection is
 * deliberately *not* one of them — `react-arborist` owns that, against its own
 * anchor and its own visible rows, which is the convention a tree widget has.
 */
export function flatten(rows: Row[]): Row[] {
	const out: Row[] = [];
	const walk = (list: Row[]): void => {
		for (const row of list) {
			out.push(row);
			if (row.children) walk(row.children);
		}
	};
	walk(rows);
	return out;
}

/** Every entity in `entity`'s subtree, itself included, parents first. */
export function subtree(rows: Row[], entity: number): number[] {
	const found = flatten(rows).find((row) => row.entity === entity);
	return found ? flatten([found]).map((row) => row.entity) : [];
}

/**
 * Whether `ancestor` is at or above `entity` in the tree.
 *
 * Used to grey out a drop target, not to decide one. The engine refuses a
 * cycle-making reparent itself and is the only answer that counts — this tree
 * is a frame old, and a second implementation of "is this a descendant" that
 * disagreed would be the drift #813 warns about. What it buys is that the drop
 * cursor never appears somewhere the drop would be refused, so the reader is
 * told before they let go rather than after.
 */
export function isAncestor(
	rows: Row[],
	ancestor: number,
	entity: number
): boolean {
	if (ancestor === entity) return true;
	const found = flatten(rows).find((row) => row.entity === ancestor);
	if (!found) return false;
	return flatten([found]).some((row) => row.entity === entity);
}

/**
 * Drop a multi-row drag down to the rows that actually have to move.
 *
 * Dragging a parent and its child together means two reparents, and the second
 * one is a lie: the child is already going wherever the parent goes, and
 * reparenting it onto the drop target as well would flatten a hierarchy the
 * reader was trying to move intact. Arborist hands over both, so this drops
 * any row whose ancestor is also in the drag.
 */
export function dragRoots(rows: Row[], entities: number[]): number[] {
	const set = new Set(entities);
	const order = flatten(rows);

	return entities.filter((entity) => {
		const row = order.find((r) => r.entity === entity);
		if (!row) return false;
		let parent = row.node.parent;
		while (parent >= 0) {
			if (set.has(parent)) return false;
			const up = order.find((r) => r.entity === parent);
			if (!up) return true;
			parent = up.node.parent;
		}
		return true;
	});
}

/**
 * Whether a row matches a search term.
 *
 * Name and id, case-insensitively. The id is in because an entity with no name
 * renders as "Entity 12" and a reader who can see that string should be able
 * to type it; searching a type would need the mask decoded into words nobody
 * has agreed on yet, and the issue only asks for it "if it is cheap".
 */
export function matches(row: Row, term: string): boolean {
	const needle = term.trim().toLowerCase();
	if (needle.length === 0) return true;
	return (
		row.name.toLowerCase().includes(needle) ||
		String(row.entity) === needle
	);
}
