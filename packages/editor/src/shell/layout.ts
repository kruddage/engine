// SPDX-License-Identifier: GPL-2.0-or-later
//
// The dock layout: what is where, what is hidden, and how wide.
//
// Pure data and pure functions. Nothing here touches React or the DOM, which is
// what lets the interesting half — reconciling a stored layout against a
// registry that has changed since it was written — be tested without rendering
// anything.
//
// ## What is persisted, and what a stale one has to survive
//
// The layout is written to localStorage on every change and read back on load.
// That means a reader's stored layout is always older than the build reading
// it, sometimes by many releases, and every one of these is normal:
//
// - a panel in the store that this build no longer has (dropped)
// - a panel in this build that the store has never heard of (placed at home)
// - a panel recorded in a slot it is no longer allowed to sit in (sent home)
// - a store from an older schema, or one another origin's app wrote (discarded)
//
// A shell that crashes on any of them is a shell that a reader fixes by
// clearing site data, which they will not know to do. reconcile() is the whole
// answer, and the suite exercises each case.

import type { PanelDefinition, PanelId } from "./panels.js";
import { DOCKABLE_SLOTS, type PanelSlot } from "./vocabulary.js";

/** One group's layout: panel id to flex-grow, as the splitter library reports it. */
export type GroupLayout = Record<string, number>;

/** Bumped when the stored shape changes incompatibly. An older one is dropped. */
export const LAYOUT_VERSION = 1;

export const LAYOUT_STORAGE_KEY = "krudd.editor.layout.v1";

export interface LayoutState {
	version: number;
	/** Panel order within each slot. A slot's first visible panel is its default tab. */
	slots: Record<PanelSlot, PanelId[]>;
	/** The raised tab per slot, or null when the slot is empty. */
	active: Partial<Record<PanelSlot, PanelId | null>>;
	/** Panels the reader has hidden. */
	hidden: PanelId[];
	/**
	 * Group layouts as react-resizable-panels reports them: group id, to panel
	 * id, to that panel's flex-grow. Stored verbatim rather than normalised —
	 * the library owns their meaning, and re-deriving it here would be a second
	 * implementation of someone else's layout algorithm.
	 *
	 * Keyed by panel id rather than positional, which is what makes a stored
	 * layout survive a panel being added, removed or moved: an id the group no
	 * longer has is simply not applied.
	 */
	sizes: Record<string, GroupLayout>;
}

function emptySlots(): Record<PanelSlot, PanelId[]> {
	return { left: [], right: [], bottom: [], deck: [] };
}

/** The layout a reader with no stored one gets: every panel at its home. */
export function defaultLayout(definitions: PanelDefinition[]): LayoutState {
	const slots = emptySlots();
	for (const definition of definitions) slots[definition.home].push(definition.id);

	return {
		version: LAYOUT_VERSION,
		slots,
		active: raiseFirst(slots, []),
		hidden: [],
		sizes: {},
	};
}

function raiseFirst(
	slots: Record<PanelSlot, PanelId[]>,
	hidden: PanelId[]
): Partial<Record<PanelSlot, PanelId | null>> {
	const active: Partial<Record<PanelSlot, PanelId | null>> = {};
	for (const slot of Object.keys(slots) as PanelSlot[]) {
		active[slot] = slots[slot].find((id) => !hidden.includes(id)) ?? null;
	}
	return active;
}

/**
 * Fold a stored layout onto the panels this build actually has.
 *
 * Always returns something usable — there is no failure mode that reaches the
 * caller, because there is nothing useful the caller could do with one.
 */
export function reconcile(
	stored: unknown,
	definitions: PanelDefinition[]
): LayoutState {
	const fallback = defaultLayout(definitions);
	if (!isStoredLayout(stored) || stored.version !== LAYOUT_VERSION) {
		return fallback;
	}

	const known = new Map(definitions.map((d) => [d.id, d]));
	const slots = emptySlots();
	const placed = new Set<PanelId>();

	for (const slot of Object.keys(slots) as PanelSlot[]) {
		for (const id of stored.slots[slot] ?? []) {
			const definition = known.get(id);
			/* Unknown panel, already placed, or parked somewhere it may no
			 * longer sit — each is dropped here and, for the third case,
			 * picked up by the home pass below. */
			if (!definition || placed.has(id)) continue;
			if (slot !== definition.home && definition.movable === false) continue;
			slots[slot].push(id);
			placed.add(id);
		}
	}

	/* Anything this build has that the store did not: place it at home, so a
	 * new panel appears for a reader with an old layout instead of silently
	 * never rendering. */
	for (const definition of definitions) {
		if (!placed.has(definition.id)) slots[definition.home].push(definition.id);
	}

	const hidden = (stored.hidden ?? []).filter(
		(id) => known.get(id)?.hideable !== false
	);

	const active = raiseFirst(slots, hidden);
	for (const slot of Object.keys(slots) as PanelSlot[]) {
		const remembered = stored.active?.[slot];
		if (
			remembered &&
			slots[slot].includes(remembered) &&
			!hidden.includes(remembered)
		) {
			active[slot] = remembered;
		}
	}

	return {
		version: LAYOUT_VERSION,
		slots,
		active,
		hidden,
		sizes: isSizes(stored.sizes) ? stored.sizes : {},
	};
}

/* ------------------------------------------------------------------ *
 * Operations. Each returns a new state; none mutates.
 * ------------------------------------------------------------------ */

export function togglePanel(state: LayoutState, id: PanelId): LayoutState {
	const hidden = state.hidden.includes(id)
		? state.hidden.filter((held) => held !== id)
		: [...state.hidden, id];
	return withActive({ ...state, hidden });
}

export function showPanel(state: LayoutState, id: PanelId): LayoutState {
	if (!state.hidden.includes(id)) return state;
	return togglePanel(state, id);
}

export function movePanel(
	state: LayoutState,
	id: PanelId,
	to: PanelSlot
): LayoutState {
	const slots = emptySlots();
	for (const slot of Object.keys(state.slots) as PanelSlot[]) {
		slots[slot] = state.slots[slot].filter((held) => held !== id);
	}
	slots[to] = [...slots[to], id];
	/* Moving a hidden panel also shows it. Sending something somewhere and
	 * having nothing appear reads as a broken menu, not as a hidden panel. */
	const hidden = state.hidden.filter((held) => held !== id);
	return withActive({ ...state, slots, hidden, active: { ...state.active, [to]: id } });
}

export function activatePanel(
	state: LayoutState,
	slot: PanelSlot,
	id: PanelId
): LayoutState {
	if (!state.slots[slot].includes(id)) return state;
	return { ...state, active: { ...state.active, [slot]: id } };
}

export function setSizes(
	state: LayoutState,
	group: string,
	sizes: GroupLayout
): LayoutState {
	return { ...state, sizes: { ...state.sizes, [group]: sizes } };
}

/** The visible panels in a slot, in order. */
export function visible(state: LayoutState, slot: PanelSlot): PanelId[] {
	return state.slots[slot].filter((id) => !state.hidden.includes(id));
}

/** Whichever panel a slot should render, or null when the slot is empty. */
export function activeIn(state: LayoutState, slot: PanelSlot): PanelId | null {
	const shown = visible(state, slot);
	if (shown.length === 0) return null;
	const remembered = state.active[slot];
	if (remembered && shown.includes(remembered)) return remembered;
	return shown[0] ?? null;
}

/*
 * Re-raise anything that a hide or a move left pointing at a panel that is no
 * longer visible in its slot. Every operation runs through here rather than
 * each remembering to do it, because the one that forgets renders an empty
 * frame with a tab bar above it.
 */
function withActive(state: LayoutState): LayoutState {
	const active = { ...state.active };
	for (const slot of Object.keys(state.slots) as PanelSlot[]) {
		active[slot] = activeIn(state, slot);
	}
	return { ...state, active };
}

/* ------------------------------------------------------------------ *
 * Persistence
 * ------------------------------------------------------------------ */

/**
 * A minimal storage face, so the suite can drive this without jsdom's
 * localStorage and without one test's writes leaking into the next.
 */
export interface LayoutStore {
	getItem: (key: string) => string | null;
	setItem: (key: string, value: string) => void;
	removeItem: (key: string) => void;
}

/**
 * localStorage, or a store that forgets.
 *
 * Access is guarded rather than assumed: a browser with site data blocked
 * throws on the *property read*, not on the call, and an editor that fails to
 * start because it could not remember a dock width would be a bad trade.
 */
export function browserStore(): LayoutStore {
	try {
		const storage = window.localStorage;
		storage.getItem(LAYOUT_STORAGE_KEY);
		return storage;
	} catch {
		return memoryStore();
	}
}

export function memoryStore(): LayoutStore {
	const held = new Map<string, string>();
	return {
		getItem: (key) => held.get(key) ?? null,
		setItem: (key, value) => void held.set(key, value),
		removeItem: (key) => void held.delete(key),
	};
}

export function loadLayout(
	store: LayoutStore,
	definitions: PanelDefinition[]
): LayoutState {
	let parsed: unknown = null;
	try {
		const raw = store.getItem(LAYOUT_STORAGE_KEY);
		parsed = raw === null ? null : JSON.parse(raw);
	} catch {
		/* Unparseable, or storage threw. Either way the default is the answer,
		 * and a reader who has never seen this shell gets the same one. */
		parsed = null;
	}
	return reconcile(parsed, definitions);
}

export function saveLayout(store: LayoutStore, state: LayoutState): void {
	try {
		store.setItem(LAYOUT_STORAGE_KEY, JSON.stringify(state));
	} catch {
		/* Quota, private mode, or a blocked origin. The layout is a
		 * convenience; losing it must not take the editor with it. */
	}
}

export function clearLayout(store: LayoutStore): void {
	try {
		store.removeItem(LAYOUT_STORAGE_KEY);
	} catch {
		/* As above. */
	}
}

/* ------------------------------------------------------------------ *
 * Shape guards
 * ------------------------------------------------------------------ */

interface StoredLayout {
	version: number;
	slots: Partial<Record<PanelSlot, PanelId[]>>;
	active?: Partial<Record<PanelSlot, PanelId | null>>;
	hidden?: PanelId[];
	sizes?: unknown;
}

function isStoredLayout(value: unknown): value is StoredLayout {
	if (typeof value !== "object" || value === null) return false;
	const candidate = value as StoredLayout;
	if (typeof candidate.version !== "number") return false;
	if (typeof candidate.slots !== "object" || candidate.slots === null) {
		return false;
	}
	for (const slot of [...DOCKABLE_SLOTS, "deck" as const]) {
		const held = candidate.slots[slot];
		if (held !== undefined && !Array.isArray(held)) return false;
	}
	if (candidate.hidden !== undefined && !Array.isArray(candidate.hidden)) {
		return false;
	}
	return true;
}

function isSizes(value: unknown): value is Record<string, GroupLayout> {
	if (typeof value !== "object" || value === null) return false;
	return Object.values(value).every(
		(entry) =>
			typeof entry === "object" &&
			entry !== null &&
			!Array.isArray(entry) &&
			Object.values(entry).every((n) => typeof n === "number")
	);
}
