// SPDX-License-Identifier: GPL-2.0-or-later
//
// Several entities, as one form.
//
// ## What "shared" has to mean
//
// #951: "multi-selection edits shared parameters; differing values are shown as
// differing and editing sets all". The word doing the work is *shared*, and the
// tempting reading — same slot, same field index — is wrong. Two entities can
// carry different meshes whose `(params ...)` clauses both declare `width` at
// different offsets, and the boundary addresses a write by index. Merging by
// index would edit `width` on one entity and `segments` on the other.
//
// So a field is shared when its **declaration** matches: name, type, component
// count, and the authored `(edit ...)` hint with its bounds. Each entity keeps
// its own (slot, field) pair, and an edit fans out to every one of them. Two
// entities whose `width` is bounded 0..1 on one asset and 0..10 on the other are
// deliberately *not* shared: one slider cannot be honest about both, and showing
// the primary's bounds would silently clamp the other's value.
//
// ## Why the primary leads
//
// Order comes from the primary — the entity `selection.id` names — so the form
// does not reshuffle as a reader ctrl-clicks more rows in. Every other selected
// entity can only remove fields from that list, never add or reorder.
//
// ## Nothing is dropped silently
//
// A field the primary declares and some other selection member does not simply
// vanishes from the panel, which would read as an asset that has fewer
// parameters than it does — the failure the `truncated` warning already exists
// to prevent. `unshared` counts them so the panel can say so.

import type { EntityParams, ParamBlock, ParamField } from "@kruddage/engine/bridge";

/** Where one field lives on one entity. The boundary's write address. */
export interface ParamTarget {
	id: number;
	/** Indexes the blocks in the order `entity.params` reported them. */
	slot: number;
	/** Indexes the fields within that block. */
	field: number;
}

export interface MergedField {
	/**
	 * The declaration every target agrees on. This is what a control is built
	 * from, so `controlFor` sees exactly what it sees in the single-selection
	 * case — the derivation does not know a multi-selection is happening.
	 */
	declaration: ParamField;
	/** The primary's value. What a control shows when the values agree. */
	value: number[];
	/** True when the targets do not all hold `value`. */
	mixed: boolean;
	targets: ParamTarget[];
}

export interface MergedBlock {
	/** "mesh" | "material" | "script". */
	slot: string;
	/** The asset behind it, or how many there are when they differ. */
	source: string;
	/** False means every target is at its declared defaults. */
	overridden: boolean;
	truncated: boolean;
	/** Fields the primary declares that the rest of the selection does not. */
	unshared: number;
	fields: MergedField[];
}

export interface MergedParams {
	blocks: MergedBlock[];
	/**
	 * Selected entities the engine has not answered for yet.
	 *
	 * Not an error and not worth blanking the panel over: a query asked this
	 * frame is answered the next. It is merged over what has arrived and
	 * counted, so the panel can say the form is still settling rather than
	 * present a partial intersection as a complete one.
	 */
	pending: number;
}

/**
 * The blocks a selection shares, with each entity's own write addresses.
 *
 * `entities` is in selection order with the primary first, one entry per
 * selected entity and null where the answer has not arrived.
 */
export function mergeParams(
	entities: readonly (EntityParams | null)[]
): MergedParams {
	const pending = entities.filter((entity) => entity === null).length;
	const primary = entities[0] ?? null;
	if (primary === null) return { blocks: [], pending };

	const others = entities.slice(1).filter((entity): entity is EntityParams =>
		entity !== null
	);

	const blocks: MergedBlock[] = [];
	for (const [slot, block] of primary.blocks.entries()) {
		/*
		 * A slot the whole selection does not carry is not shown at all. A
		 * "Material" tab over a selection where only one entity has a material
		 * would be a tab whose every edit reached one entity — worse than the
		 * tab not being there, because it looks like it reached all of them.
		 */
		const matched = matchBlocks(others, block.slot);
		if (matched === null) continue;

		const merged = mergeBlock(primary.id, slot, block, matched);
		blocks.push(merged);
	}

	return { blocks, pending };
}

/**
 * Each entity's block for `slot`, or null when one of them has no such block.
 *
 * The pairs carry the block's index because that index is what a write is
 * addressed by, and it is the entity's own — the same slot can sit at a
 * different position on an entity with no mesh.
 */
function matchBlocks(
	entities: readonly EntityParams[],
	slot: string
): { id: number; slot: number; block: ParamBlock }[] | null {
	const matched: { id: number; slot: number; block: ParamBlock }[] = [];
	for (const entity of entities) {
		const index = entity.blocks.findIndex((block) => block.slot === slot);
		const block = entity.blocks[index];
		if (block === undefined) return null;
		matched.push({ id: entity.id, slot: index, block });
	}
	return matched;
}

function mergeBlock(
	primaryId: number,
	primarySlot: number,
	primary: ParamBlock,
	others: readonly { id: number; slot: number; block: ParamBlock }[]
): MergedBlock {
	const fields: MergedField[] = [];
	for (const [index, declaration] of primary.fields.entries()) {
		const targets: ParamTarget[] = [
			{ id: primaryId, slot: primarySlot, field: index },
		];
		let mixed = false;
		let shared = true;

		for (const other of others) {
			const at = other.block.fields.findIndex((candidate) =>
				sameDeclaration(declaration, candidate)
			);
			const field = other.block.fields[at];
			if (field === undefined) {
				shared = false;
				break;
			}
			targets.push({ id: other.id, slot: other.slot, field: at });
			mixed ||= !sameValue(declaration.value, field.value);
		}

		if (!shared) continue;
		fields.push({
			declaration,
			value: declaration.value,
			mixed,
			targets,
		});
	}

	return {
		slot: primary.slot,
		source: sourceOf([primary, ...others.map((other) => other.block)]),
		/* Any target off its defaults means the block as shown is not at
		 * them — the note says "at defaults" only when that is true of all. */
		overridden: [primary, ...others.map((other) => other.block)].some(
			(block) => block.overridden
		),
		truncated: [primary, ...others.map((other) => other.block)].some(
			(block) => block.truncated
		),
		unshared: primary.fields.length - fields.length,
		fields,
	};
}

/**
 * Whether two declarations are the same parameter.
 *
 * The bounds are part of it, not decoration: a control built from one
 * declaration and applied to a field declared with different bounds would clamp
 * the second entity's value to a range its asset never asked for.
 */
function sameDeclaration(a: ParamField, b: ParamField): boolean {
	return (
		a.name === b.name &&
		a.type === b.type &&
		a.components === b.components &&
		a.edit === b.edit &&
		Object.is(a.min, b.min) &&
		Object.is(a.max, b.max)
	);
}

/*
 * Exact comparison, deliberately. Both numbers came from the same engine
 * through the same decode, so two values that are "the same" are bit-identical;
 * an epsilon here would report two entities as agreeing when a script had moved
 * one of them by a hair, and the reader would edit both without knowing.
 */
function sameValue(a: readonly number[], b: readonly number[]): boolean {
	if (a.length !== b.length) return false;
	return a.every((value, index) => Object.is(value, b[index]));
}

function sourceOf(blocks: readonly ParamBlock[]): string {
	const paths = new Set(blocks.map((block) => block.path ?? "unknown asset"));
	const only = [...paths][0];
	if (paths.size === 1 && only !== undefined) return only;
	return `${paths.size} assets`;
}
