// SPDX-License-Identifier: GPL-2.0-or-later
//
// The command bar's menus, and every keyboard shortcut, declared once.
//
// ## Reimplemented from `editor_layout.scm`, never read from it
//
// The Scheme spec is 103 lines and it is still the best description of what the
// chrome should contain — so it was mined for this table, line by line, and
// then closed. **Nothing here reads it at runtime.** #944's Q3 ruled that a
// TypeScript app resolving its own menu set out of a Scheme file at boot is the
// worst of both worlds, and #953 retires the file.
//
// The one thing carried over verbatim is the `&` mnemonic in each label, and it
// is carried for a reason rather than out of faithfulness: the hint an unwired
// action shows is the label with the mnemonic stripped, and keeping the raw
// form is what makes that derivation checkable instead of hand-written twice.
//
// ## Which actions are real
//
// Everything the editor can currently do: the layout actions, and — once a
// document exists — undo, redo, new, open and save. The hint path survives for
// the rest (cut, copy, paste, quit), and it is what keeps an unwired menu
// honest instead of silent. #953 is where the last of them either arrive or are
// listed as dropped.
//
// Note where the document actions are *not*: there is no branch here that
// mutates a scene. `runAction` dispatches a command id and the document layer
// owns what that means, so this file names actions and never performs them.

import type { PanelId } from "./panels.js";
import type { PanelSlot } from "./vocabulary.js";

/** The standard-key symbols `editor_layout.scm` uses, and nothing else. */
export type StandardKey =
	| "new"
	| "open"
	| "save"
	| "save-as"
	| "quit"
	| "undo"
	| "redo"
	| "cut"
	| "copy"
	| "paste"
	| "none";

/**
 * A shortcut as a platform-neutral chord. "Mod" is Command on a Mac and Control
 * everywhere else — resolved at display time and at match time from the same
 * table, so the two can never disagree about what a menu promises.
 */
export interface Chord {
	mod?: boolean;
	shift?: boolean;
	alt?: boolean;
	key: string;
}

const STANDARD_KEYS: Record<Exclude<StandardKey, "none">, Chord> = {
	new: { mod: true, key: "n" },
	open: { mod: true, key: "o" },
	save: { mod: true, key: "s" },
	"save-as": { mod: true, shift: true, key: "s" },
	quit: { mod: true, key: "q" },
	undo: { mod: true, key: "z" },
	redo: { mod: true, shift: true, key: "z" },
	cut: { mod: true, key: "x" },
	copy: { mod: true, key: "c" },
	paste: { mod: true, key: "v" },
};

export interface ActionItem {
	kind: "action";
	/** The label as authored, mnemonic and all. */
	label: string;
	shortcut: StandardKey;
	/** Opaque id. One the shell does not handle falls through to the hint. */
	id: string;
}

export interface SeparatorItem {
	kind: "separator";
}

/** Expands to one show/hide toggle per registered panel, in registry order. */
export interface PanelTogglesItem {
	kind: "panel-toggles";
}

/** Expands to a submenu per movable panel, each listing the dockable slots. */
export interface PanelMovesItem {
	kind: "panel-moves";
}

export type MenuItem =
	| ActionItem
	| SeparatorItem
	| PanelTogglesItem
	| PanelMovesItem;

export interface Menu {
	label: string;
	items: MenuItem[];
}

const action = (label: string, shortcut: StandardKey, id: string): ActionItem => ({
	kind: "action",
	label,
	shortcut,
	id,
});

const separator: SeparatorItem = { kind: "separator" };

/**
 * The menu set. Same order, same labels, same shortcuts as the Scheme spec.
 *
 * View gains a Move submenu the spec has no equivalent for: `editor_layout.scm`
 * describes a fixed arrangement, and a shell whose panels can be re-slotted
 * needs somewhere to say so. It is the only addition.
 */
export const MENUS: readonly Menu[] = [
	{
		label: "&File",
		items: [
			action("&New Project", "new", "new"),
			action("&Open Project…", "open", "open-project"),
			separator,
			action("&Save", "save", "save"),
			action("Save &As…", "save-as", "save-as"),
			separator,
			action("&Quit", "quit", "quit"),
		],
	},
	{
		label: "&Edit",
		items: [
			action("&Undo", "undo", "undo"),
			action("&Redo", "redo", "redo"),
			separator,
			action("Cu&t", "cut", "cut"),
			action("&Copy", "copy", "copy"),
			action("&Paste", "paste", "paste"),
		],
	},
	{
		label: "&View",
		items: [
			{ kind: "panel-toggles" },
			separator,
			{ kind: "panel-moves" },
			separator,
			action("Reset &Layout", "none", "reset-layout"),
		],
	},
	{
		label: "&Help",
		items: [action("&About krudd", "none", "about")],
	},
];

/** The toolbar, mirroring the spec: a renderer badge, elastic gap, version. */
export const TOOLBAR_BADGES = ["renderer", "version"] as const;

/**
 * Strip a label's `&` mnemonic marker.
 *
 * "Save &As…" becomes "Save As…". A doubled `&&` is a literal ampersand, which
 * no label uses today and which costs one branch to keep correct rather than
 * discovering later that "Cut && Paste" renders as "Cut  Paste".
 */
export function stripMnemonic(label: string): string {
	/* Split on the escape first so a doubled marker survives the strip that
	 * follows, rather than needing a sentinel character in the source. */
	return label
		.split("&&")
		.map((part) => part.replace(/&/g, ""))
		.join("&");
}

/** The chord an action's standard key stands for, or null for `none`. */
export function chordFor(shortcut: StandardKey): Chord | null {
	return shortcut === "none" ? null : STANDARD_KEYS[shortcut];
}

/**
 * How a chord is written for a reader.
 *
 * Apple's glyphs on a Mac, words everywhere else, because those are the two
 * conventions readers already know and inventing a third helps nobody.
 */
export function chordLabel(chord: Chord | null, mac = isMac()): string {
	if (!chord) return "";
	const parts: string[] = [];
	if (chord.mod) parts.push(mac ? "⌘" : "Ctrl");
	if (chord.shift) parts.push(mac ? "⇧" : "Shift");
	if (chord.alt) parts.push(mac ? "⌥" : "Alt");
	parts.push(chord.key.toUpperCase());
	return mac ? parts.join("") : parts.join("+");
}

/** Whether a keydown is this chord. */
export function chordMatches(
	chord: Chord,
	event: Pick<KeyboardEvent, "key" | "shiftKey" | "altKey" | "ctrlKey" | "metaKey">,
	mac = isMac()
): boolean {
	const mod = mac ? event.metaKey : event.ctrlKey;
	if (Boolean(chord.mod) !== mod) return false;
	if (Boolean(chord.shift) !== event.shiftKey) return false;
	if (Boolean(chord.alt) !== event.altKey) return false;
	/*
	 * Compared case-insensitively because Shift changes `key`: the redo chord
	 * arrives as "Z", not "z", and matching the literal would leave every
	 * shifted shortcut in the table silently dead.
	 */
	return event.key.toLowerCase() === chord.key.toLowerCase();
}

export function isMac(): boolean {
	if (typeof navigator === "undefined") return false;
	return /mac/i.test(navigator.platform || navigator.userAgent);
}

/** Every action in the menu tree, flattened. The shortcut table reads this. */
export function allActions(menus: readonly Menu[] = MENUS): ActionItem[] {
	return menus.flatMap((menu) =>
		menu.items.filter((item): item is ActionItem => item.kind === "action")
	);
}

/** What the shell can actually do. Anything else falls through to the hint. */
export interface CommandContext {
	resetLayout: () => void;
	togglePanel: (id: PanelId) => void;
	movePanel: (id: PanelId, slot: PanelSlot) => void;
	hint: (text: string) => void;
	/**
	 * The document actions, absent until the engine has booted.
	 *
	 * Optional rather than always-present because there is a real window —
	 * every frame before the wasm runtime comes up, and forever on a page whose
	 * engine was never built — in which Undo genuinely cannot work. During it,
	 * the menu items fall through to the hint path and say so, which is the
	 * same thing they do for an action nobody has written yet.
	 */
	document?: DocumentActions | undefined;
}

/**
 * The document, as the menu needs it: five verbs and nothing else.
 *
 * Deliberately not the KruddDocument itself. The shell has no business
 * dispatching an arbitrary command or opening a gesture, and handing it the
 * whole object would make "the shell does not mutate the scene" a convention
 * rather than something the types enforce.
 */
export interface DocumentActions {
	undo: () => void;
	redo: () => void;
	newProject: () => void;
	open: () => void;
	save: () => void;
}

/**
 * Run an action id.
 *
 * The default branch is the point of the function, not an oversight: an id this
 * shell has not wired shows the label with its mnemonic stripped and the words
 * "coming soon", which is what `shell.html.in:1548` does today and what the
 * native host does for an unrecognised id. Silence would read as a broken menu.
 */
export function runAction(
	id: string,
	label: string,
	context: CommandContext
): void {
	if (id === "reset-layout") {
		context.resetLayout();
		context.hint("layout reset");
		return;
	}

	const document = context.document;
	if (document) {
		switch (id) {
			case "undo":
				document.undo();
				return;
			case "redo":
				document.redo();
				return;
			case "new":
				document.newProject();
				return;
			case "open-project":
				document.open();
				return;
			case "save":
				document.save();
				return;
			default:
				break;
		}
	}

	context.hint(`${stripMnemonic(label)}: coming soon`);
}
