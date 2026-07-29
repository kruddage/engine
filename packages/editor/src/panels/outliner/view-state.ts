// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the outliner remembers that the scene does not.
//
// #950 asks two questions out loud — where hidden/locked lives and whether it
// survives a save, and how long expansion state lasts — and asks for the
// answers to be stated rather than arrived at. They are stated here.
//
// ## Hidden and locked: engine state, editor memory
//
// The flags themselves live in the engine, because the engine is what draws
// and what picks. An editor-side "hidden" would dim a row while the entity
// carried on rendering, which is not hiding it. So `entity.flags` is a command
// like any other and `scene.tree` reports the result.
//
// But they are **not document state**, and the engine agrees: it never writes
// them to a scene file, never captures them in an undo snapshot, and clears
// them when a scene is ingested. **So a save does not carry them, and the
// answer to "does it survive a save" is no.** A level that shipped with
// something invisible nobody could find is the failure that decides it.
//
// What *does* survive is this module: the flags are mirrored into the browser
// and replayed onto the engine the first time the panel sees a scene. That is
// the honest place for it — it is a preference about how one person is looking
// at a scene, and it belongs beside their dock layout rather than in the file
// they are about to commit.
//
// The mirror is memory, never truth. What a row renders comes from the engine's
// `scene.tree`, exactly as the highlight comes from the engine's selection; if
// the two ever disagreed, the engine would be right. Storing the authority here
// instead would be the same mistake in a quieter place.
//
// ## Expansion: neither, and not here at all
//
// Expansion state belongs to `react-arborist`'s own store, which is the right
// place for it: it is a property of the list widget, nothing else reads it, and
// mirroring it would be state kept in two places to no end. It therefore
// survives a selection change for free — the tree is not remounted by one —
// which is #950's criterion.
//
// It does **not** survive a scene load, and that takes one line rather than
// none: the panel keys the tree on `DocumentState.loads`, so a loaded document
// remounts it and the open set goes with the world it described. Restoring
// "row 12 was open" onto a scene whose id 12 is something else entirely is the
// failure that rules out persisting it.

import {
	browserStore,
	type LayoutStore,
	memoryStore,
} from "../../shell/layout.js";
import { ENTITY_FLAG } from "../../document/commands.js";

/*
 * Versioned like the layout key, and for the same reason: the shape stored
 * here is this build's, a reader's copy is always older than the build reading
 * it, and a key that changed meaning without changing name is how a stale
 * value becomes a bug nobody can reproduce.
 */
export const FLAGS_STORAGE_KEY = "krudd.editor.outliner.flags.v1";

/* Re-exported so a caller gets a store without also importing the shell — the
 * outliner is a panel, and the panel's storage is the same browser storage the
 * layout uses, not a second mechanism. */
export { browserStore, memoryStore };
export type { LayoutStore };

export interface FlagMirror {
	/** Entity id to its ENTITY_FLAG bits. Only non-zero entries are kept. */
	[entity: number]: number;
}

export const HIDDEN = ENTITY_FLAG.HIDDEN;
export const LOCKED = ENTITY_FLAG.LOCKED;

/** Whether a flag word has a bit. */
export function has(flags: number, bit: number): boolean {
	return (flags & bit) !== 0;
}

/** The flag word with one bit toggled. */
export function toggle(flags: number, bit: number): number {
	return has(flags, bit) ? flags & ~bit : flags | bit;
}

/**
 * Read the remembered flags.
 *
 * Every failure answers "nothing remembered" rather than throwing: storage is
 * unavailable in a private window, the value is whatever a previous build
 * wrote, and a reader is not going to know that clearing site data is what
 * fixes an outliner that will not open. Same discipline as the shell's layout
 * store (`src/shell/layout.ts`), and for the same reason.
 */
export function readFlags(store: LayoutStore): FlagMirror {
	try {
		const raw = store.getItem(FLAGS_STORAGE_KEY);
		if (!raw) return {};
		const parsed: unknown = JSON.parse(raw);
		if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
			return {};
		}

		const out: FlagMirror = {};
		for (const [key, value] of Object.entries(parsed)) {
			const id = Number.parseInt(key, 10);
			if (!Number.isInteger(id) || id < 0) continue;
			if (typeof value !== "number" || !Number.isInteger(value)) continue;
			/* Bits this build does not know are dropped rather than replayed
			 * onto the engine, which would set flags nothing can clear. */
			const bits = value & (HIDDEN | LOCKED);
			if (bits !== 0) out[id] = bits;
		}
		return out;
	} catch {
		return {};
	}
}

/** Remember the flags. Silent when storage is unavailable. */
export function writeFlags(store: LayoutStore, mirror: FlagMirror): void {
	try {
		/* Zero entries are dropped rather than stored: the mirror is a sparse
		 * overlay on "everything visible and unlocked", and keeping the
		 * defaults would grow it by one entry per entity a reader ever
		 * touched. */
		const kept = Object.fromEntries(
			Object.entries(mirror).filter(([, bits]) => bits !== 0)
		);
		store.setItem(FLAGS_STORAGE_KEY, JSON.stringify(kept));
	} catch {
		/* Nothing to do and nothing to say. A reader who cannot persist a
		 * preference should still be able to use the panel. */
	}
}
