// SPDX-License-Identifier: GPL-2.0-or-later
//
// The vocabulary, checked.
//
// A vocabulary nothing verifies is a vocabulary that drifts on the second PR —
// which is exactly what #948 says the naming exists to prevent, and exactly
// what happened to #886's mockup. These assertions are cheap and they are the
// only thing standing between a settled vocabulary and a list in a comment.

import { describe, expect, it } from "vitest";

import { registerBuiltinPanels } from "../src/panels/index.js";
import { panels } from "../src/shell/panels.js";
import { DOCKABLE_SLOTS, SLOT_LABEL, VOCABULARY } from "../src/shell/vocabulary.js";

const TERMS = [
	"command bar",
	"toolbar",
	"rail",
	"viewport deck",
	"outliner",
	"inspector",
	"assets",
	"console",
	"build",
	"status strip",
] as const;

describe("the vocabulary", () => {
	it("defines exactly the terms #948 settled, and no synonyms", () => {
		expect(Object.keys(VOCABULARY).sort()).toEqual([...TERMS].sort());
	});

	it("gives every term one line, not a placeholder", () => {
		for (const [term, definition] of Object.entries(VOCABULARY)) {
			expect(definition.length, term).toBeGreaterThan(20);
			expect(definition, term).toMatch(/\.$/);
		}
	});

	it("names every panel from the vocabulary", () => {
		/* A panel called something the vocabulary does not contain is how a
		 * second name for one thing gets in. */
		registerBuiltinPanels();
		for (const definition of panels()) {
			const term = definition.id === "viewport" ? "viewport deck" : definition.id;
			expect(VOCABULARY, definition.id).toHaveProperty(term);
		}
	});

	it("labels every dockable slot", () => {
		for (const slot of DOCKABLE_SLOTS) {
			expect(SLOT_LABEL[slot]).toBeTruthy();
		}
	});

	it("does not offer the deck as somewhere a panel can be moved", () => {
		/*
		 * Not a layout preference. The engine looks its canvas up by selector,
		 * and moving the viewport between slots would remount it and hand the
		 * engine a detached element — losing the GL context and every pointer
		 * callback with it.
		 */
		expect(DOCKABLE_SLOTS).not.toContain("deck");
	});
});
