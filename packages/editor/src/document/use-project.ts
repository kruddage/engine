// SPDX-License-Identifier: GPL-2.0-or-later
//
// New, Open, Save, Undo, Redo — the five verbs the command bar drives.
//
// This is the seam between #948's shell and #947's document, and it is
// deliberately thin. The shell knows five functions and a hint sink; the
// document knows commands and the engine. Neither knows the other, which is
// what stops the menu from growing scene logic and the document from growing a
// dependency on how a menu reports failure.
//
// ## Everything here reports
//
// Each verb ends in a hint. That is not chrome — three of these five can fail
// in ways a reader must not have to guess at: a save the engine refused, a save
// storage would not take, an open with nothing stored. The read-only shell
// established that an action which cannot happen says so in the status strip,
// and an action that *did* happen should be no quieter.

import { useCallback, useMemo } from "react";

import type { DocumentActions } from "../shell/commands.js";
import type { KruddDocument } from "./document.js";
import {
	EMPTY_SCENE,
	loadProject,
	saveProject,
	type ProjectStore,
} from "./project-store.js";

export interface UseProjectOptions {
	document: KruddDocument | null;
	store: ProjectStore;
	hint: (text: string) => void;
}

/**
 * The five verbs, or undefined when there is no document to drive.
 *
 * Undefined rather than a set of no-ops: the shell's hint path already says
 * "coming soon" for an action it cannot perform, and routing a dead Undo
 * through a silent no-op would be strictly worse than the message a reader
 * already understands.
 */
export function useProject(
	options: UseProjectOptions
): DocumentActions | undefined {
	const { document, store, hint } = options;

	const open = useCallback(
		(text: string, what: string) => {
			if (!document) return;
			document
				.load(text)
				.then(() => hint(what))
				.catch((error: unknown) => hint(`open failed: ${reason(error)}`));
		},
		[document, hint]
	);

	const save = useCallback(() => {
		if (!document) return;
		document
			.save()
			.then((text) => {
				/*
				 * The store's answer decides the message. A save that failed on
				 * quota and reported "saved" is the one failure here that costs
				 * a reader real work, and it is invisible until they reload.
				 */
				hint(
					saveProject(store, text)
						? "project saved"
						: "save failed: storage refused the write"
				);
			})
			.catch((error: unknown) => hint(`save failed: ${reason(error)}`));
	}, [document, store, hint]);

	return useMemo(() => {
		if (!document) return undefined;
		return {
			undo: () => document.dispatch("history.undo", {}),
			redo: () => document.dispatch("history.redo", {}),
			newProject: () => open(EMPTY_SCENE, "new project"),
			open: () => {
				const text = loadProject(store);
				if (text === null) {
					hint("no saved project to open");
					return;
				}
				open(text, "project opened");
			},
			save,
		};
	}, [document, store, hint, open, save]);
}

function reason(error: unknown): string {
	return error instanceof Error ? error.message : String(error);
}
