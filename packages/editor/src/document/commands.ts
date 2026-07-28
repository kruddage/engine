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

import type {
	Bridge,
	DragPhase,
	GizmoMode,
	TransformLike,
} from "@kruddage/engine/bridge";

/*
 * The boundary's enumerations, re-exported.
 *
 * `src/document/` is the only place allowed to import the bridge for its values
 * (test/document-boundary.test.ts holds that against the source tree), and
 * these are values: a panel that wants to say "translate" needs the number the
 * C side means by it. Re-exporting keeps one door rather than making an
 * exception to the rule for constants — and a second copy of the numbers here
 * would be a second place for them to be wrong.
 */
export {
	DRAG_PHASE,
	GIZMO_AXIS,
	GIZMO_MODE,
} from "@kruddage/engine/bridge";

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
	"entity.param": { id: number; slot: number; field: number; value: number[] };
	"history.undo": Record<string, never>;
	"history.redo": Record<string, never>;
	"engine.paused": { paused: boolean };
	"document.load": { text: string };

	/* The viewport (#949). */
	"viewport.size": { width: number; height: number };
	"viewport.pick": { x: number; y: number };
	"camera.orbit": { dyaw: number; dpitch: number };
	"camera.pan": { dx: number; dy: number };
	"camera.dolly": { amount: number };
	"camera.frame": { id?: number };
	"camera.reset": Record<string, never>;
	"engine.editing": { editing: boolean };
	"gizmo.mode": { mode: GizmoMode };
	"gizmo.snap": { translate: number; rotate: number; scale: number };
	"gizmo.drag": { phase: DragPhase; x: number; y: number };
	"gizmo.grid": { shown: boolean; spacing: number };
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
	/*
	 * Continuous, because a slider drag repeats it once a frame. The engine
	 * coalesces consecutive same-key edits into one history entry, so what
	 * this owes that mechanism is a gesture around the drag — not a
	 * coalescing rule of its own (#944, Q2).
	 */
	"entity.param": {
		label: "Set Parameter",
		continuity: "continuous",
		run: (bridge, { id, slot, field, value }) =>
			bridge.setParam(id, slot, field, value),
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

	/* ------------------------------------------------------------------ *
	 * The viewport (#949)
	 *
	 * Every one of these is here rather than called from the viewport panel
	 * for the reason the file's header gives: `run` is the only place a
	 * bridge mutator is called from in the whole application, and
	 * test/document-boundary.test.ts holds that true against the source
	 * tree. A camera orbit is not a scene mutation, but exempting it would
	 * mean the rule was "mutations, plus whatever else someone decides is
	 * not one".
	 * ------------------------------------------------------------------ */

	/*
	 * Not an edit, and it is the one command here that is pure bookkeeping:
	 * it tells the engine how big the canvas is so the ray it casts and the
	 * pixels it drew agree. A drag repeats it — a splitter drag resizes the
	 * dock every frame — so it is continuous, though there is nothing for
	 * the history to coalesce.
	 */
	"viewport.size": {
		label: "Resize Viewport",
		continuity: "continuous",
		run: (bridge, { width, height }) =>
			bridge.setViewportSize(width, height),
	},
	/*
	 * A pick *is* a selection: the engine raycasts and calls set_selected
	 * itself, and the answer arrives through #947's selection query like
	 * any other. The editor never holds an id the engine has not agreed to,
	 * which is why this is not "ask what is there, then select it".
	 */
	"viewport.pick": {
		label: "Select",
		continuity: "discrete",
		run: (bridge, { x, y }) => bridge.pick(x, y),
	},

	/*
	 * The camera gestures. Continuous, all of them — a drag repeats them
	 * once a frame — but nothing they do is undoable, because the camera is
	 * not part of the document. Undoing an edit must not move the view, and
	 * that falls out of the camera never reaching `world/edit` at all.
	 */
	"camera.orbit": {
		label: "Orbit",
		continuity: "continuous",
		run: (bridge, { dyaw, dpitch }) => bridge.orbitCamera(dyaw, dpitch),
	},
	"camera.pan": {
		label: "Pan",
		continuity: "continuous",
		run: (bridge, { dx, dy }) => bridge.panCamera(dx, dy),
	},
	"camera.dolly": {
		label: "Zoom",
		continuity: "continuous",
		run: (bridge, { amount }) => bridge.dollyCamera(amount),
	},
	/* -1, the default, means "the selection" — resolved by the engine so a
	 * shortcut cannot disagree with it about what is selected. */
	"camera.frame": {
		label: "Frame Selection",
		continuity: "discrete",
		run: (bridge, { id }) => bridge.frameCamera(id ?? -1),
	},
	"camera.reset": {
		label: "Reset View",
		continuity: "discrete",
		run: (bridge) => bridge.resetCamera(),
	},

	/*
	 * The play/edit handover, and the whole of it: the engine pauses the
	 * simulation, lights the selection outline and stands kruddgui's
	 * click-to-pick down on the way in, and undoes all three on the way out.
	 * Nothing on this side is torn down or rebuilt, which is what makes it
	 * survive being pressed repeatedly.
	 */
	"engine.editing": {
		label: "Edit Mode",
		continuity: "discrete",
		run: (bridge, { editing }) => bridge.setEditorMode(editing),
	},

	"gizmo.mode": {
		label: "Gizmo Mode",
		continuity: "discrete",
		run: (bridge, { mode }) => bridge.setGizmoMode(mode),
	},
	"gizmo.snap": {
		label: "Snap",
		continuity: "discrete",
		run: (bridge, { translate, rotate, scale }) =>
			bridge.setGizmoSnap(translate, rotate, scale),
	},
	/*
	 * A drag phase. The moves land on the engine's set_transform, so they
	 * coalesce into the gesture the viewport opened at pointerdown — one
	 * undo step per drag, by the same mechanism an inspector slider uses
	 * (#944, Q2). Continuous for exactly that reason.
	 */
	"gizmo.drag": {
		label: "Transform",
		continuity: "continuous",
		run: (bridge, { phase, x, y }) => bridge.gizmoDrag(phase, x, y),
	},
	"gizmo.grid": {
		label: "Grid",
		continuity: "discrete",
		run: (bridge, { shown, spacing }) => bridge.setGrid(shown, spacing),
	},
};

/** Whether a string names a command. Narrows, so callers can dispatch it. */
export function isCommandId(id: string): id is CommandId {
	return Object.hasOwn(COMMANDS, id);
}
