// SPDX-License-Identifier: GPL-2.0-or-later
//
// Where a saved project goes.
//
// ## localStorage, and what that is and is not
//
// One slot, holding the `(scene ...)` text the engine wrote. That is enough to
// make Save and Open round-trip a document, which is what #947 asks for, and it
// is deliberately not a file.
//
// **Files are not here, and that is a scope decision rather than an oversight.**
// Reading and writing real files means a picker, a format on disk, and the
// desktop drag-and-drop affordance #944's non-goals are explicitly nervous
// about — and #952 owns the question of whether file import exists at all, with
// an acceptance criterion requiring it to be answered out loud. Answering it
// here, in passing, in the PR that builds the command layer, is how a
// deliberately deferred question gets settled by accident.
//
// So: one slot, and the storage layer is an interface, so the day a file picker
// or an IndexedDB-backed project list arrives it replaces this and nothing
// above it changes.
//
// The store interface is the same shape as the layout's, and for the same
// reason — a browser with site data blocked throws on the property read, and an
// editor that will not start because it could not remember a project is a worse
// outcome than one that forgets.

import { browserStore, memoryStore, type LayoutStore } from "../shell/layout.js";

/** Reused wholesale: a key-value store of strings is a key-value store. */
export type ProjectStore = LayoutStore;

export const PROJECT_STORAGE_KEY = "krudd.editor.project.v1";

export { browserStore, memoryStore };

/** The stored project's text, or null when there is none. */
export function loadProject(store: ProjectStore): string | null {
	try {
		return store.getItem(PROJECT_STORAGE_KEY);
	} catch {
		return null;
	}
}

/**
 * Store `text`. Returns whether it stuck.
 *
 * The return value is not decoration: a save that silently failed on quota is
 * the one failure mode that costs a reader real work, so the caller says so in
 * the status strip rather than showing "saved" over a write that did not
 * happen. That is the whole reason this does not just throw away the error the
 * way saveLayout does — a dock width is a convenience and a project is not.
 */
export function saveProject(store: ProjectStore, text: string): boolean {
	try {
		store.setItem(PROJECT_STORAGE_KEY, text);
		return true;
	} catch {
		return false;
	}
}

export function clearProject(store: ProjectStore): void {
	try {
		store.removeItem(PROJECT_STORAGE_KEY);
	} catch {
		/* Nothing to do, and nothing worth telling the reader about. */
	}
}

/**
 * An empty document.
 *
 * A scene form rather than an empty string: New has to go through the same load
 * path as Open, so that clearing the world, resetting the history and marking
 * the document clean all happen exactly once, in the engine, in the order the
 * load op already defines.
 */
export const EMPTY_SCENE = "(scene untitled)\n";
