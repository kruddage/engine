// SPDX-License-Identifier: GPL-2.0-or-later
//
// Every mutation the editor can make, as data.
//
// ## Why a table and not methods
//
// #947 asks that every scene mutation be "a command object: describable,
// undoable, serializable", and that no panel mutate the scene directly. A
// method per mutation would satisfy the last part and none of the others: a
// call is not describable, so a menu item cannot render its label, and it is
// not serializable, so nothing can log what a session did.
//
// A table gives all three for free. `label` is what the command bar shows and
// what the engine records as the gesture name. `{id, payload}` is JSON, so a
// command is loggable and replayable. And `run` is the only place a bridge
// mutator is called from in the whole application — which is what makes the
// "no panel writes directly" rule checkable rather than aspirational
// (document.test.ts asserts it against the source tree).
//
// ## Undoable is not this file's business
//
// There is deliberately no undo machinery here, no memento and no history.
// #944's Q2 settled it: `world/edit`'s 128-entry ring is the only undo stack
// in the system, and undo and redo are commands like any other — the toolbar
// dispatches `history.undo` and waits for the events, it does not pop
// anything locally. What a command carries about undo is its label and, for
// the continuous ones, the fact that it belongs inside a gesture.

import type { Bridge, TransformLike } from "@kruddage/engine/bridge";

/**
 * Whether a command is one a drag repeats.
 *
 * The engine coalesces consecutive same-key edits into one history entry, so a
 * slider drag is one undo step whether it came from the inspector or a C-side
 * gizmo. What the editor owes that mechanism is a gesture around the drag —
 * this flag is how a caller knows a command wants one.
 */
export type Continuity = "discrete" | "continuous";

export interface CommandSpec<P> {
	/** Shown in menus, and recorded as the engine's gesture label. */
	readonly label: string;
	readonly continuity: Continuity;
	readonly run: (bridge: Bridge, payload: P) => void;
}

export interface CommandPayloads {
	"selection.set": { id: number };
	"entity.create": { parent: number; transform?: TransformLike; mask?: number };
	"entity.destroy": { id: number };
	"entity.transform": { id: number; transform: TransformLike };
	"entity.rename": { id: number; name: string };
	"entity.mesh": { id: number; ref: number };
	"entity.material": { id: number; ref: number };
	"entity.script": { id: number; ref: number };
	"history.undo": Record<string, never>;
	"history.redo": Record<string, never>;
	"engine.paused": { paused: boolean };
	"document.load": { text: string };
}

export type CommandId = keyof CommandPayloads;

type Table = { [K in CommandId]: CommandSpec<CommandPayloads[K]> };

/*
 * The identity transform, spelled out.
 *
 * A created entity with no transform gets this rather than a zeroed struct:
 * the tape encoder already defaults a missing scale to 1, and repeating the
 * rule here means the two cannot drift into an entity that spawns collapsed to
 * a point on one path and not the other.
 */
const IDENTITY: TransformLike = {
	position: [0, 0, 0],
	rotation: [0, 0, 0, 1],
	scale: [1, 1, 1],
};

export const COMMANDS: Table = {
	/*
	 * Selection is a command, not a local setState. It is the one that most
	 * invites an exception — nothing about a selection is undoable, and
	 * mirroring it in TypeScript would render a frame sooner. It stays a
	 * command because the engine's selection is what a C-side gizmo and a
	 * script both read, and a second copy here would be the "two views that
	 * disagree" failure #947 exists to prevent.
	 */
	"selection.set": {
		label: "Select",
		continuity: "discrete",
		run: (bridge, { id }) => bridge.select(id),
	},
	"entity.create": {
		label: "Create Entity",
		continuity: "discrete",
		run: (bridge, { parent, transform, mask }) =>
			bridge.createEntity(parent, transform ?? IDENTITY, mask ?? 0),
	},
	"entity.destroy": {
		label: "Delete Entity",
		continuity: "discrete",
		run: (bridge, { id }) => bridge.destroyEntity(id),
	},
	"entity.transform": {
		label: "Move",
		continuity: "continuous",
		run: (bridge, { id, transform }) => bridge.setTransform(id, transform),
	},
	"entity.rename": {
		label: "Rename",
		continuity: "discrete",
		run: (bridge, { id, name }) => bridge.setName(id, name),
	},
	"entity.mesh": {
		label: "Assign Mesh",
		continuity: "discrete",
		run: (bridge, { id, ref }) => bridge.setRenderRef(id, ref),
	},
	"entity.material": {
		label: "Assign Material",
		continuity: "discrete",
		run: (bridge, { id, ref }) => bridge.setMaterialRef(id, ref),
	},
	"entity.script": {
		label: "Assign Script",
		continuity: "discrete",
		run: (bridge, { id, ref }) => bridge.setScriptRef(id, ref),
	},
	"history.undo": {
		label: "Undo",
		continuity: "discrete",
		run: (bridge) => bridge.undo(),
	},
	"history.redo": {
		label: "Redo",
		continuity: "discrete",
		run: (bridge) => bridge.redo(),
	},
	"engine.paused": {
		label: "Pause",
		continuity: "discrete",
		run: (bridge, { paused }) => bridge.setPaused(paused),
	},
	/*
	 * Loading is a mutation like any other from the editor's side, and unlike
	 * any other from the history's: the engine clears the undo ring behind it,
	 * because a memento captured against the old world names entity ids that
	 * now mean something else. Nothing here has to know that — it is stated so
	 * the absence of a "this is not undoable" flag reads as deliberate.
	 */
	"document.load": {
		label: "Open Project",
		continuity: "discrete",
		run: (bridge, { text }) => bridge.loadScene(text),
	},
};

/** Whether a string names a command. Narrows, so callers can dispatch it. */
export function isCommandId(id: string): id is CommandId {
	return Object.hasOwn(COMMANDS, id);
}
