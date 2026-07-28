// SPDX-License-Identifier: GPL-2.0-or-later
//
// Keyboard shortcuts, bound from the same table the menus render.
//
// **Declared in one place** is the acceptance criterion, and this file is what
// makes it true rather than aspirational: there is no chord literal here. Every
// binding comes from `commands.ts`'s action table, so a shortcut that a menu
// advertises and a shortcut that actually fires cannot drift apart — adding an
// action to the table binds it, and removing one unbinds it.

import { useEffect } from "react";

import { allActions, chordFor, chordMatches, isMac, type ActionItem } from "./commands.js";

/**
 * Bind every action in `actions` that declares a chord.
 *
 * A match calls `onAction(id, label)` and swallows the event. Swallowing is
 * deliberate and it is most of the value: Ctrl+S must not open the browser's
 * save dialog over an editor that has its own Save, and an editor that lets it
 * through is one a reader learns to avoid using the keyboard in.
 */
export function useShortcuts(
	onAction: (id: string, label: string) => void,
	actions: ActionItem[] = allActions()
): void {
	useEffect(() => {
		const mac = isMac();

		const onKeyDown = (event: KeyboardEvent): void => {
			/*
			 * Typing is not a shortcut. Ctrl+C in a rename field is a copy, and
			 * an editor that intercepts it to run its own Copy action is one
			 * that cannot be typed in. Checked here, once, rather than in every
			 * panel that grows a text field later.
			 */
			if (isTextEntry(event.target)) return;

			for (const item of actions) {
				const chord = chordFor(item.shortcut);
				if (!chord || !chordMatches(chord, event, mac)) continue;
				event.preventDefault();
				onAction(item.id, item.label);
				return;
			}
		};

		window.addEventListener("keydown", onKeyDown);
		return () => window.removeEventListener("keydown", onKeyDown);
	}, [onAction, actions]);
}

function isTextEntry(target: EventTarget | null): boolean {
	if (!(target instanceof HTMLElement)) return false;
	if (target.isContentEditable) return true;
	const tag = target.tagName;
	return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT";
}
