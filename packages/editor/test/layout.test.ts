// SPDX-License-Identifier: GPL-2.0-or-later
//
// The dock layout, without rendering one.
//
// Most of this file is about a stored layout that is older than the build
// reading it. That case is not exotic — it is what every reader gets on every
// release after their first — and it is the one where getting it wrong means a
// reader whose editor is broken until they think to clear site data.

import { describe, expect, it } from "vitest";

import {
	LAYOUT_STORAGE_KEY,
	LAYOUT_VERSION,
	activatePanel,
	activeIn,
	clearLayout,
	defaultLayout,
	loadLayout,
	memoryStore,
	movePanel,
	reconcile,
	saveLayout,
	setSizes,
	showPanel,
	togglePanel,
	visible,
} from "../src/shell/layout.js";
import type { PanelDefinition } from "../src/shell/panels.js";

const stub = (): React.JSX.Element => null as unknown as React.JSX.Element;

const PANELS: PanelDefinition[] = [
	{ id: "viewport", title: "Viewport", home: "deck", hideable: false, movable: false, render: stub },
	{ id: "outliner", title: "Outliner", home: "left", render: stub },
	{ id: "inspector", title: "Inspector", home: "right", render: stub },
	{ id: "assets", title: "Assets", home: "bottom", render: stub },
	{ id: "console", title: "Console", home: "bottom", render: stub },
];

describe("the default layout", () => {
	it("puts every panel at its home and raises the first in each slot", () => {
		const layout = defaultLayout(PANELS);

		expect(layout.slots.left).toEqual(["outliner"]);
		expect(layout.slots.right).toEqual(["inspector"]);
		expect(layout.slots.bottom).toEqual(["assets", "console"]);
		expect(layout.slots.deck).toEqual(["viewport"]);
		expect(layout.active.bottom).toBe("assets");
		expect(layout.hidden).toEqual([]);
	});
});

describe("reconciling a stored layout", () => {
	it("keeps a slotting the reader chose", () => {
		const stored = movePanel(defaultLayout(PANELS), "console", "right");
		const layout = reconcile(stored, PANELS);

		expect(layout.slots.right).toEqual(["inspector", "console"]);
		expect(layout.slots.bottom).toEqual(["assets"]);
	});

	it("drops a panel this build no longer has", () => {
		const stored = defaultLayout(PANELS);
		stored.slots.left.push("timeline" as never);

		expect(reconcile(stored, PANELS).slots.left).toEqual(["outliner"]);
	});

	it("places a panel the store has never heard of at its home", () => {
		/* The case every new panel hits on a reader who has used the editor
		 * before: without this it would be registered, and never render. */
		const stored = defaultLayout(PANELS.filter((p) => p.id !== "inspector"));
		const layout = reconcile(stored, PANELS);

		expect(layout.slots.right).toEqual(["inspector"]);
	});

	it("sends an immovable panel home from wherever it was recorded", () => {
		const stored = defaultLayout(PANELS);
		stored.slots.deck = [];
		stored.slots.left = ["outliner", "viewport"];

		const layout = reconcile(stored, PANELS);
		expect(layout.slots.deck).toEqual(["viewport"]);
		expect(layout.slots.left).toEqual(["outliner"]);
	});

	it("refuses to hide a panel that may not be hidden", () => {
		const stored = defaultLayout(PANELS);
		stored.hidden = ["viewport"];

		expect(reconcile(stored, PANELS).hidden).toEqual([]);
	});

	it("falls back to the default on a version it does not know", () => {
		const stored = { ...defaultLayout(PANELS), version: LAYOUT_VERSION + 1 };
		expect(reconcile(stored, PANELS)).toEqual(defaultLayout(PANELS));
	});

	it.each([null, undefined, 7, "a string", [], { version: 1 }])(
		"falls back to the default on %s",
		(stored) => {
			expect(reconcile(stored, PANELS).slots.left).toEqual(["outliner"]);
		}
	);

	it("re-raises a tab that is no longer in its slot", () => {
		const stored = defaultLayout(PANELS);
		stored.active.bottom = "inspector";

		expect(reconcile(stored, PANELS).active.bottom).toBe("assets");
	});
});

describe("operations", () => {
	it("hides and shows a panel", () => {
		const hidden = togglePanel(defaultLayout(PANELS), "assets");
		expect(visible(hidden, "bottom")).toEqual(["console"]);
		/* The slot re-raises rather than rendering a tab strip over nothing. */
		expect(activeIn(hidden, "bottom")).toBe("console");

		expect(visible(togglePanel(hidden, "assets"), "bottom")).toEqual([
			"assets",
			"console",
		]);
	});

	it("reports no active panel for a slot with everything hidden", () => {
		let layout = defaultLayout(PANELS);
		layout = togglePanel(layout, "assets");
		layout = togglePanel(layout, "console");

		expect(activeIn(layout, "bottom")).toBeNull();
	});

	it("shows a panel that a move sends somewhere", () => {
		/* Sending something to a slot and having nothing appear reads as a
		 * broken menu, not as a hidden panel. */
		const hidden = togglePanel(defaultLayout(PANELS), "console");
		const moved = movePanel(hidden, "console", "left");

		expect(moved.hidden).not.toContain("console");
		expect(activeIn(moved, "left")).toBe("console");
	});

	it("leaves a panel in exactly one slot after a move", () => {
		const moved = movePanel(defaultLayout(PANELS), "outliner", "bottom");
		const everywhere = Object.values(moved.slots).flat();

		expect(everywhere.filter((id) => id === "outliner")).toHaveLength(1);
	});

	it("showPanel is a no-op on a panel that is already shown", () => {
		const layout = defaultLayout(PANELS);
		expect(showPanel(layout, "assets")).toBe(layout);
	});

	it("ignores activating a panel that is not in the slot", () => {
		const layout = defaultLayout(PANELS);
		expect(activatePanel(layout, "left", "assets")).toBe(layout);
		expect(activatePanel(layout, "bottom", "console").active.bottom).toBe(
			"console"
		);
	});
});

describe("persistence", () => {
	it("round-trips through a store", () => {
		const store = memoryStore();
		const wanted = setSizes(
			movePanel(defaultLayout(PANELS), "console", "left"),
			"columns",
			{ left: 1.5, centre: 4, right: 2 }
		);

		saveLayout(store, wanted);
		expect(loadLayout(store, PANELS)).toEqual(wanted);
	});

	it("returns the default when nothing is stored", () => {
		expect(loadLayout(memoryStore(), PANELS)).toEqual(defaultLayout(PANELS));
	});

	it("returns the default when the stored value is not JSON", () => {
		const store = memoryStore();
		store.setItem(LAYOUT_STORAGE_KEY, "{not json");

		expect(loadLayout(store, PANELS)).toEqual(defaultLayout(PANELS));
	});

	it("survives a store that throws on every call", () => {
		/*
		 * Site data blocked, private mode, or a quota that is already full. The
		 * layout is a convenience and losing it must not take the editor with
		 * it — so all three of these have to be non-events.
		 */
		const hostile = {
			getItem: () => {
				throw new Error("blocked");
			},
			setItem: () => {
				throw new Error("blocked");
			},
			removeItem: () => {
				throw new Error("blocked");
			},
		};

		expect(loadLayout(hostile, PANELS)).toEqual(defaultLayout(PANELS));
		expect(() => saveLayout(hostile, defaultLayout(PANELS))).not.toThrow();
		expect(() => clearLayout(hostile)).not.toThrow();
	});

	it("clears rather than overwrites, so a reset survives a reload", () => {
		const store = memoryStore();
		saveLayout(store, movePanel(defaultLayout(PANELS), "console", "left"));
		clearLayout(store);

		expect(store.getItem(LAYOUT_STORAGE_KEY)).toBeNull();
		expect(loadLayout(store, PANELS)).toEqual(defaultLayout(PANELS));
	});

	it("discards sizes that are not the shape the splitter reports", () => {
		const store = memoryStore();
		store.setItem(
			LAYOUT_STORAGE_KEY,
			JSON.stringify({ ...defaultLayout(PANELS), sizes: { columns: [1, 2] } })
		);

		expect(loadLayout(store, PANELS).sizes).toEqual({});
	});
});
