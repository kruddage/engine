// SPDX-License-Identifier: GPL-2.0-or-later
//
// The document: what the editor thinks it is editing, and how a change is made.
//
// ## It is a view, and it holds nothing
//
// #944's Q1 decided this and it is worth repeating where the temptation lives:
// **the engine owns the document, and this is a view over it.** Reads are
// queries answered out of the bridge's generation-stamped cache. Writes are
// commands the engine executes. Changes arrive as events. Nothing here is
// authoritative about anything, and there is no field on this object that a
// panel could read instead of asking the engine.
//
// So there is no store. What looks like one — `save`, `dirty` — is bookkeeping
// about the *file*, not about the scene: the scene generation at the last save.
// It is the only state this layer keeps, and it keeps it because the engine has
// no concept of a file to keep it for us.
//
// ## No undo stack, on purpose
//
// #944's Q2: `world/edit`'s 128-entry ring is the only history in the system,
// and undo() and redo() here are command dispatches. There is no local stack to
// keep honest, no mementos, and no coalescing logic — the engine coalesces by
// key, so a slider drag inside a gesture is one entry whether the drag started
// in the inspector or on a C-side gizmo. That equivalence is free rather than
// engineered, and it is the whole payoff of the decision.
//
// ## The frame loop
//
// One flush per frame, always the same call. Commands drain before the tick,
// events after. A command and the events it produces never straddle a frame
// boundary, which is what bounds the cost of having no optimistic UI at one
// frame — the same frame a C-side gizmo would have moved in.

import type { Bridge, BridgeReply } from "@kruddage/engine/bridge";

import { COMMANDS, type CommandId, type CommandPayloads } from "./commands.js";

/**
 * Drives the per-frame flush. Returns a function that stops it.
 *
 * Injected rather than assumed so a test can step the loop by hand: a suite
 * that has to wait for real animation frames to observe a command is a suite
 * that is slow and flaky for no reason.
 */
export type Scheduler = (tick: () => void) => () => void;

const frameScheduler: Scheduler = (tick) => {
	let handle = requestAnimationFrame(function frame() {
		tick();
		handle = requestAnimationFrame(frame);
	});
	return () => cancelAnimationFrame(handle);
};

export interface DocumentOptions {
	bridge: Bridge;
	scheduler?: Scheduler | undefined;
}

export interface DocumentState {
	/** Whether the scene has changed since the last save or load. */
	dirty: boolean;
	/** How many gestures are open. Non-zero during a drag. */
	gestureDepth: number;
	/**
	 * How many documents have been loaded into this session.
	 *
	 * Not a version and not a generation — the scene's generation is the
	 * engine's and already crosses the boundary. This counts one specific
	 * event: **the world was replaced**, so every entity id now means
	 * something else. A panel holding anything keyed by id — the outliner's
	 * expanded rows (#950) — has to drop it, and there is otherwise no signal
	 * that says so: a load moves the scene generation exactly as an edit does.
	 */
	loads: number;
}

export interface KruddDocument {
	readonly bridge: Bridge;

	/** Start the per-frame flush. Returns a function that stops it. */
	run: () => () => void;

	/** Run a command. The engine applies it on the next flush. */
	dispatch: <K extends CommandId>(id: K, payload: CommandPayloads[K]) => void;

	/**
	 * Bracket a run of commands into one undo entry.
	 *
	 * `label` is what the history shows. The returned handle commits or aborts
	 * exactly once; calling either twice is a no-op rather than an unbalanced
	 * begin, because a pointerup handler that fires twice is a browser
	 * behaviour and not something every caller should have to defend against.
	 */
	gesture: (label: string) => Gesture;

	/** The file-level state. Changes notify subscribers. */
	state: () => DocumentState;

	/**
	 * The whole scene as text, and the point at which it stops being dirty.
	 *
	 * Rejects rather than resolving with null when the engine could not write
	 * one: a caller that wrote the resolved value to a file would otherwise
	 * save the word "null" over a project.
	 */
	save: () => Promise<string>;

	/** Replace the document. Resolves once the engine has applied it. */
	load: (text: string) => Promise<void>;

	/** Called after every reply that moved a generation or carried events. */
	subscribe: (listener: (reply: BridgeReply) => void) => () => void;

	/**
	 * Called when `state()` changes to a new object, and not otherwise.
	 *
	 * Separate from subscribe() because the two answer different questions and
	 * fire at different times: a reply arrives every frame something moved,
	 * while dirtiness and gesture depth change rarely and also change outside
	 * a reply — committing a gesture is not an exchange.
	 */
	subscribeState: (listener: () => void) => () => void;
}

export interface Gesture {
	commit: () => void;
	abort: () => void;
}

export function createDocument(options: DocumentOptions): KruddDocument {
	const { bridge, scheduler = frameScheduler } = options;

	/*
	 * The scene generation this document is "clean" at.
	 *
	 * Null until the engine has answered once, and then seeded with whatever
	 * it first reported rather than with 0. The baseline is the world as the
	 * editor found it: an editor that opens dirty, before the reader has
	 * touched anything, has taught them to ignore the marker by lunchtime.
	 */
	let savedGeneration: number | null = null;
	let gestureDepth = 0;
	let loads = 0;

	const listeners = new Set<(reply: BridgeReply) => void>();
	const stateListeners = new Set<() => void>();
	let state: DocumentState = { dirty: false, gestureDepth: 0, loads: 0 };

	/*
	 * A new object only when something actually changed.
	 *
	 * useSyncExternalStore compares snapshots by identity and re-renders when
	 * they differ, so publishing a fresh object every frame would rerender
	 * every panel reading this sixty times a second to show the same two
	 * booleans. The early return is what makes state() safe to use as a
	 * snapshot at all.
	 */
	function publish(next: Partial<DocumentState>): void {
		const merged = { ...state, ...next };
		if (merged.dirty === state.dirty &&
		    merged.gestureDepth === state.gestureDepth &&
		    merged.loads === state.loads) {
			return;
		}
		state = merged;
		for (const listener of stateListeners) listener();
	}

	bridge.subscribe((reply) => {
		savedGeneration ??= bridge.generations.scene;
		publish({
			dirty: bridge.generations.scene !== savedGeneration,
			gestureDepth,
		});
		for (const listener of listeners) listener(reply);
	});

	function dispatch<K extends CommandId>(
		id: K,
		payload: CommandPayloads[K]
	): void {
		COMMANDS[id].run(bridge, payload);
	}

	function gesture(label: string): Gesture {
		bridge.beginGesture(label);
		gestureDepth += 1;
		let settled = false;

		const end = (finish: () => void) => () => {
			if (settled) return;
			settled = true;
			finish();
			gestureDepth -= 1;
			publish({ dirty: state.dirty, gestureDepth });
		};

		publish({ dirty: state.dirty, gestureDepth });
		return {
			commit: end(() => bridge.commitGesture()),
			abort: end(() => bridge.abortGesture()),
		};
	}

	async function save(): Promise<string> {
		const text = await bridge.ask("scene.text");
		if (typeof text !== "string") {
			throw new Error(
				"the engine could not write this scene — see the console for why"
			);
		}
		/*
		 * The generation the text describes, read after the answer rather than
		 * before the ask: the flush that answered may also have applied
		 * commands queued in the same frame, and those are in the text.
		 */
		savedGeneration = bridge.generations.scene;
		publish({ dirty: false, gestureDepth });
		return text;
	}

	async function load(text: string): Promise<void> {
		dispatch("document.load", { text });
		/*
		 * A ride-along query, awaited for its timing rather than its value.
		 *
		 * It resolves on the flush whose tape carried the load, which is
		 * exactly when the world has changed — and unlike waiting for a
		 * subscriber to fire, it is still true when the loaded scene happens
		 * to be identical to the one already open. That case moves no
		 * generation, notifies nobody, and would hang a promise that waited
		 * for a change. `selection` is the cheapest thing to ask for.
		 */
		await bridge.ask("selection");
		savedGeneration = bridge.generations.scene;
		loads += 1;
		publish({ dirty: false, gestureDepth, loads });
	}

	return {
		bridge,
		run: () => scheduler(() => bridge.flush()),
		dispatch,
		gesture,
		state: () => state,
		save,
		load,
		subscribe: (listener) => {
			listeners.add(listener);
			return () => listeners.delete(listener);
		},
		subscribeState: (listener) => {
			stateListeners.add(listener);
			return () => stateListeners.delete(listener);
		},
	};
}
