// SPDX-License-Identifier: GPL-2.0-or-later
//
// The document, from inside a panel — and the only way a panel reaches it.
//
// ## Why every read goes through useSyncExternalStore
//
// The bridge's cache is the store, and it is updated outside React by the
// frame loop. That is precisely the case useSyncExternalStore exists for, and
// using it rather than a useState mirror is not a style preference: a mirror
// would be a second copy of engine state living in React, which is the design
// #944's Q1 rejected. Here the snapshot *is* `bridge.read(...)`, so a panel
// cannot hold a value the engine has moved on from.
//
// The cache returns the same object while the generation holds — the engine
// answers `fresh` and no new value is parsed — so identity comparison does the
// right thing for free and a panel re-renders when its data changes and not
// once a frame.
//
// ## Why watch() lives in subscribe()
//
// A query is only asked while something is interested. Registering the watch
// inside the subscribe callback ties that interest to the component's lifetime
// exactly: mounted means watched, unmounted means the query stops riding every
// tape. Doing it in a separate effect would work and would drift — two effects
// with the same dependencies is how a panel ends up watching something after it
// is gone.

import { createContext, useCallback, useContext, useSyncExternalStore } from "react";

import type {
	EntityDetail,
	HistoryState,
	QueryKind,
	QueryValues,
	SceneTree,
	SelectionState,
} from "@kruddage/engine/bridge";

import type { DocumentState, KruddDocument } from "./document.js";

const DocumentContext = createContext<KruddDocument | null>(null);

export const DocumentProvider = DocumentContext.Provider;

/**
 * The document, from inside a panel.
 *
 * Throws rather than returning null: a panel rendered outside the provider is a
 * wiring mistake, and every call site would otherwise carry a null check for a
 * state that cannot legitimately occur.
 */
export function useDocument(): KruddDocument {
	const document = useOptionalDocument();
	if (document === null) {
		throw new Error("a panel was rendered outside the DocumentProvider");
	}
	return document;
}

/**
 * The document, or null.
 *
 * There is a real window in which there is none: every frame before the wasm
 * runtime comes up, forever on a page whose engine was never built, and after a
 * protocol mismatch. A panel that reads engine state has to render *something*
 * during it, so it asks this and says what it knows — rather than throwing,
 * which would take the whole shell down for a condition that is ordinary.
 *
 * useDocument() stays the strict one, for the code paths that only run once a
 * document exists.
 */
export function useOptionalDocument(): KruddDocument | null {
	return useContext(DocumentContext);
}

/**
 * Watch one query and re-render when its answer changes.
 *
 * The generic read. The named hooks below are this with a kind baked in, and
 * they exist so a panel says what it wants rather than what it is watching.
 */
export function useQuery<K extends QueryKind>(
	kind: K,
	id = -1
): QueryValues[K] | null {
	const document = useOptionalDocument();

	const subscribe = useCallback(
		(onChange: () => void) => {
			if (!document) return () => {};
			const unwatch = document.bridge.watch(kind, id);
			const off = document.subscribe(onChange);
			return () => {
				off();
				unwatch();
			};
		},
		[document, kind, id]
	);

	return useSyncExternalStore(
		subscribe,
		() => document?.bridge.read(kind, id) ?? null,
		/*
		 * The server snapshot. There is no server — the editor is a client-only
		 * app — but the argument is required when a component might hydrate,
		 * and null is the honest answer for "the engine has not spoken yet".
		 */
		() => null
	);
}

/** The scene as a flat list of entities, parent-before-child. */
export function useSceneTree(): SceneTree | null {
	return useQuery("scene.tree");
}

/**
 * The selection. One owner, and this is how everything reads it.
 *
 * Note what this is not: a piece of React state a panel sets. Selecting is a
 * command (`selection.set`), the engine applies it, and every view — including
 * a C-side gizmo — sees the same answer one flush later. Two panels cannot
 * disagree about what is selected because neither of them holds it.
 */
export function useSelection(): SelectionState | null {
	return useQuery("selection");
}

/** One entity in full, transforms included. Null while it is unknown or gone. */
export function useEntity(id: number): EntityDetail | null {
	return useQuery("entity", id);
}

/** What undo and redo would do next. The engine's ring, read. */
export function useHistory(): HistoryState | null {
	return useQuery("history");
}

/** Dirtiness and gesture depth — the file-level state, not the scene's. */
export function useDocumentState(): DocumentState {
	const document = useDocument();

	return useSyncExternalStore(
		document.subscribeState,
		document.state,
		document.state
	);
}
