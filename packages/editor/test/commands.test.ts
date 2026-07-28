// SPDX-License-Identifier: GPL-2.0-or-later
//
// The command table: the menu set, the shortcuts, and the hint.
//
// The first block is the one that matters most. `editor_layout.scm` was mined
// for this table and then closed, so nothing checks the two against each other
// at runtime any more — which means the only thing standing between "we
// reimplemented the menu set" and "we reimplemented most of the menu set" is an
// explicit list here.

import { describe, expect, it, vi } from "vitest";

import {
	MENUS,
	allActions,
	chordFor,
	chordLabel,
	chordMatches,
	runAction,
	stripMnemonic,
	type CommandContext,
} from "../src/shell/commands.js";

/*
 * Transcribed from krudd/engine/core/editor_layout.scm as it stands. A menu
 * added there and not here is a divergence that is now deliberate — #953
 * retires that file — but it should be a decision someone made, not one this
 * suite let through in silence.
 */
const SCHEME_SPEC = {
	"&File": [
		["&New Project", "new", "new"],
		["&Open Project…", "open", "open-project"],
		["&Save", "save", "save"],
		["Save &As…", "save-as", "save-as"],
		["&Quit", "quit", "quit"],
	],
	"&Edit": [
		["&Undo", "undo", "undo"],
		["&Redo", "redo", "redo"],
		["Cu&t", "cut", "cut"],
		["&Copy", "copy", "copy"],
		["&Paste", "paste", "paste"],
	],
	"&View": [["Reset &Layout", "none", "reset-layout"]],
	"&Help": [["&About krudd", "none", "about"]],
} as const;

describe("the menu set", () => {
	it("has the same menus, in the same order, as the Scheme spec", () => {
		expect(MENUS.map((menu) => menu.label)).toEqual(Object.keys(SCHEME_SPEC));
	});

	it.each(Object.entries(SCHEME_SPEC))(
		"reimplements every %s action with its label, shortcut and id",
		(label, expected) => {
			const menu = MENUS.find((candidate) => candidate.label === label);
			const actions = allActions(menu ? [menu] : []).map((item) => [
				item.label,
				item.shortcut,
				item.id,
			]);

			expect(actions).toEqual(expected.map((row) => [...row]));
		}
	);

	it("declares every shortcut exactly once", () => {
		const chords = allActions()
			.map((item) => chordLabel(chordFor(item.shortcut), false))
			.filter((label) => label !== "");

		expect(new Set(chords).size).toBe(chords.length);
	});
});

describe("mnemonics", () => {
	it.each([
		["&New Project", "New Project"],
		["Save &As…", "Save As…"],
		["Cu&t", "Cut"],
		["&About krudd", "About krudd"],
		["no mnemonic", "no mnemonic"],
		/* A doubled marker is a literal ampersand. No label uses one today,
		 * and getting it wrong would render "Cut && Paste" as "Cut  Paste". */
		["Cut && Paste", "Cut & Paste"],
	])("strips %s to %s", (raw, stripped) => {
		expect(stripMnemonic(raw)).toBe(stripped);
	});
});

describe("chords", () => {
	it("writes words off a Mac and glyphs on one", () => {
		expect(chordLabel(chordFor("save-as"), false)).toBe("Ctrl+Shift+S");
		expect(chordLabel(chordFor("save-as"), true)).toBe("⌘⇧S");
		expect(chordLabel(chordFor("none"), false)).toBe("");
	});

	it("matches Command on a Mac and Control everywhere else", () => {
		const save = chordFor("save");
		const ctrlS = { key: "s", ctrlKey: true, metaKey: false, shiftKey: false, altKey: false };
		const metaS = { key: "s", ctrlKey: false, metaKey: true, shiftKey: false, altKey: false };

		expect(chordMatches(save!, ctrlS, false)).toBe(true);
		expect(chordMatches(save!, metaS, false)).toBe(false);
		expect(chordMatches(save!, metaS, true)).toBe(true);
	});

	it("matches a shifted chord, whose key arrives upper-cased", () => {
		/* The browser reports "Z", not "z", once Shift is down. Matching the
		 * literal would leave every shifted shortcut in the table dead. */
		const redo = chordFor("redo");
		expect(
			chordMatches(
				redo!,
				{ key: "Z", ctrlKey: true, metaKey: false, shiftKey: true, altKey: false },
				false
			)
		).toBe(true);
	});

	it("does not match the unshifted chord when Shift is down", () => {
		const save = chordFor("save");
		expect(
			chordMatches(
				save!,
				{ key: "S", ctrlKey: true, metaKey: false, shiftKey: true, altKey: false },
				false
			)
		).toBe(false);
	});
});

describe("running an action", () => {
	const context = (): CommandContext => ({
		resetLayout: vi.fn(),
		togglePanel: vi.fn(),
		movePanel: vi.fn(),
		hint: vi.fn(),
	});

	it("resets the layout, and says so", () => {
		const ctx = context();
		runAction("reset-layout", "Reset &Layout", ctx);

		expect(ctx.resetLayout).toHaveBeenCalledOnce();
		expect(ctx.hint).toHaveBeenCalledWith("layout reset");
	});

	it("hints for an id it has not wired, with the mnemonic stripped", () => {
		/* This is the shape of an honest read-only shell: an unwired action
		 * says so rather than doing nothing and looking broken. */
		const ctx = context();
		runAction("save-as", "Save &As…", ctx);

		expect(ctx.hint).toHaveBeenCalledWith("Save As…: coming soon");
	});

	it("hints for every action this shell has not wired", () => {
		const ctx = context();
		for (const item of allActions()) runAction(item.id, item.label, ctx);

		const hinted = (ctx.hint as ReturnType<typeof vi.fn>).mock.calls.filter(
			([text]) => String(text).endsWith(": coming soon")
		);
		/* Every action except Reset Layout, which is the only wired one. */
		expect(hinted).toHaveLength(allActions().length - 1);
	});
});
