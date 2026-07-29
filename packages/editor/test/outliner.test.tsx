// SPDX-License-Identifier: GPL-2.0-or-later
//
// The outliner panel (#950).
//
// The load-bearing tests here are the two that decide whether this is a view of
// the scene or a view of the editor's intentions:
//
//   - "an entity the editor did not create appears without a refresh"
//   - "a selection the outliner did not make repaints its rows"
//
// Everything else is the criteria list. What is deliberately *not* here is
// virtualization and real drag-and-drop: jsdom reports every element as
// zero-sized and has no drag data transfer, so a test that claimed either
// would be lying (#944, Q6). The drop *decision* is a pure function and lives
// in outliner-tree.test.ts; how many rows a 5,000-entity tree keeps in the DOM
// is Playwright's question, and it is stated as such in the README rather than
// asserted here badly.

import { act, fireEvent, render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import type { SceneTree, SelectionState, TreeNode } from "@kruddage/engine/bridge";

import { DocumentProvider } from "../src/document/document-context.js";
import type { KruddDocument } from "../src/document/document.js";
import { Outliner } from "../src/panels/outliner/outliner.js";
import { memoryStore } from "../src/shell/layout.js";

/* ------------------------------------------------------------------ *
 * A document whose reads a test controls
 * ------------------------------------------------------------------ */

function node(
	id: number,
	parent: number,
	name: string | null = null,
	flags = 0
): TreeNode {
	return { id, parent, name, mask: 0, render: 0, material: 0, script: 0, flags };
}

interface Stub {
	document: KruddDocument;
	dispatch: ReturnType<typeof vi.fn>;
	commit: ReturnType<typeof vi.fn>;
	gestures: string[];
	/** Change what the engine answers with, and wake the subscribers. */
	answer: (next: { tree?: SceneTree; selection?: SelectionState }) => void;
}

/**
 * A document whose reads are canned and whose writes are recorded.
 *
 * The bridge itself is exercised by the engine package's suite and by
 * bridge_test.c. What is under test here is the panel: that it asks for the
 * right things, dispatches the right commands, and renders what came back
 * rather than what it sent.
 */
function stubDocument(initial: {
	tree?: SceneTree;
	selection?: SelectionState;
}): Stub {
	const dispatch = vi.fn();
	const commit = vi.fn();
	const gestures: string[] = [];
	const listeners = new Set<() => void>();

	let tree = initial.tree ?? { paused: false, entities: [] };
	let selection = initial.selection ?? { id: -1, ids: [] };
	/*
	 * One object, not a fresh one per call. useSyncExternalStore compares
	 * snapshots by identity, and a getSnapshot that allocated would re-render
	 * forever — which is exactly what the real document guards against in
	 * publish(), so a stub that did not would be testing against a document
	 * shape that does not exist.
	 */
	const documentState = { dirty: false, gestureDepth: 0, loads: 0 };

	const document = {
		bridge: {
			watch: () => () => {},
			read: (kind: string) =>
				kind === "scene.tree" ? tree : kind === "selection" ? selection : null,
		},
		dispatch,
		gesture: (label: string) => {
			gestures.push(label);
			return { commit, abort: vi.fn() };
		},
		subscribe: (listener: () => void) => {
			listeners.add(listener);
			return () => listeners.delete(listener);
		},
		subscribeState: () => () => {},
		state: () => documentState,
		run: () => () => {},
		save: vi.fn(),
		load: vi.fn(),
	} as unknown as KruddDocument;

	return {
		document,
		dispatch,
		commit,
		gestures,
		answer: (next) => {
			if (next.tree) tree = next.tree;
			if (next.selection) selection = next.selection;
			/* What a real flush does: the cached value moved, so everything
			 * watching it re-reads. Nothing tells the panel *what* changed.
			 * Wrapped in act() because the notification comes from outside
			 * React, which is exactly what the frame loop does in the app. */
			act(() => {
				for (const listener of listeners) listener();
			});
		},
	};
}

function show(initial: {
	tree?: SceneTree;
	selection?: SelectionState;
}): Stub {
	const stub = stubDocument(initial);
	render(
		<DocumentProvider value={stub.document}>
			<Outliner store={memoryStore()} />
		</DocumentProvider>
	);
	return stub;
}

/*
 * root(0)
 *   crate(1)
 *     lid(3)
 *   lamp(2)
 */
const SCENE: SceneTree = {
	paused: false,
	entities: [
		node(0, -1, "root"),
		node(1, 0, "crate"),
		node(2, 0, "lamp"),
		node(3, 1, "lid"),
	],
};

function row(entity: number): HTMLElement {
	return screen.getByTestId(`outliner-row-${entity}`);
}

/* ------------------------------------------------------------------ *
 * Tests
 * ------------------------------------------------------------------ */

describe("the outliner panel", () => {
	it("says so plainly before the engine is up", () => {
		render(<Outliner store={memoryStore()} />);
		expect(screen.getByTestId("outliner-waiting")).toBeTruthy();
	});

	it("says so plainly when the scene is empty", () => {
		show({});
		expect(screen.getByTestId("outliner-empty")).toBeTruthy();
	});

	it("renders the hierarchy, and says how much of it there is", () => {
		show({ tree: SCENE });
		expect(row(0)).toBeTruthy();
		expect(row(3)).toBeTruthy();
		expect(screen.getByTestId("outliner-count").textContent).toContain(
			"4 entities"
		);
	});

	/* ---------------------------------------------------------------- *
	 * Selection
	 * ---------------------------------------------------------------- */

	it("selects through a command rather than through local state", () => {
		const { dispatch } = show({ tree: SCENE });

		fireEvent.click(row(1));
		expect(dispatch.mock.calls).toEqual([["selection.set", { id: 1 }]]);
		/* And the row is not selected yet, because the engine has not said so.
		 * A panel that highlighted optimistically would be holding a selection
		 * the engine might refuse. */
		expect(row(1).getAttribute("data-selected")).toBe("false");
	});

	it("highlights from the engine's answer", () => {
		show({ tree: SCENE, selection: { id: 1, ids: [1, 2] } });
		expect(row(1).getAttribute("data-selected")).toBe("true");
		expect(row(2).getAttribute("data-selected")).toBe("true");
		expect(row(0).getAttribute("data-selected")).toBe("false");
		expect(screen.getByTestId("outliner-count").textContent).toContain(
			"2 selected"
		);
	});

	it("repaints when a selection it did not make arrives", () => {
		/*
		 * The bidirectional criterion, from the direction that is easy to get
		 * wrong. Nothing in this test touches the outliner: the selection moves
		 * the way a viewport pick, a script or an undo moves it, and the rows
		 * have to follow.
		 */
		const stub = show({ tree: SCENE, selection: { id: -1, ids: [] } });
		expect(row(3).getAttribute("data-selected")).toBe("false");

		stub.answer({ selection: { id: 3, ids: [3] } });
		expect(row(3).getAttribute("data-selected")).toBe("true");
		/* And it did not send a command to say so — that would be the outliner
		 * echoing the engine's own answer back at it. */
		expect(stub.dispatch).not.toHaveBeenCalled();
	});

	it("adds to the selection with the platform modifier", () => {
		const { dispatch } = show({ tree: SCENE, selection: { id: 1, ids: [1] } });

		fireEvent.click(row(2), { ctrlKey: true });
		/* Set-then-add, because the engine is told the whole set rather than a
		 * delta — it holds the selection and this side holds none of it. */
		expect(dispatch.mock.calls).toEqual([
			["selection.set", { id: 1 }],
			["selection.add", { id: 2 }],
		]);
	});

	it("removes a row the modifier lands on when it is already selected", () => {
		const { dispatch } = show({
			tree: SCENE,
			selection: { id: 2, ids: [1, 2] },
		});

		fireEvent.click(row(2), { metaKey: true });
		/* Down to just row 1 — the modifier toggled row 2 back off. */
		expect(dispatch.mock.calls).toEqual([["selection.set", { id: 1 }]]);
	});

	it("selects a range with shift, primary last", () => {
		const { dispatch } = show({ tree: SCENE });

		fireEvent.click(row(1));            /* the anchor */
		dispatch.mockClear();
		fireEvent.click(row(2), { shiftKey: true });

		/*
		 * Set-then-add rather than one command with a list, and the order
		 * matters: the engine's primary is whatever was added last, so the row
		 * the reader shift-clicked ends up being the one the gizmo acts on.
		 *
		 * The range is the library's — its own anchor, its own visible rows —
		 * which is why the middle id is 3 rather than 2: root is open, so lid
		 * sits between crate and lamp on screen.
		 */
		expect(dispatch.mock.calls).toEqual([
			["selection.set", { id: 1 }],
			["selection.add", { id: 3 }],
			["selection.add", { id: 2 }],
		]);
	});

	/* ---------------------------------------------------------------- *
	 * Edits
	 * ---------------------------------------------------------------- */

	it("renames through a command", () => {
		const { dispatch } = show({ tree: SCENE });

		fireEvent.doubleClick(screen.getByText("crate"));
		const input = screen.getByTestId("outliner-rename-1");
		fireEvent.keyDown(input, { key: "Enter", currentTarget: input });

		expect(dispatch).toHaveBeenCalledWith(
			"entity.rename",
			expect.objectContaining({ id: 1 })
		);
	});

	it("deletes the whole selection as one undo step", () => {
		const stub = show({ tree: SCENE, selection: { id: 2, ids: [1, 2] } });

		fireEvent.click(screen.getByTestId("outliner-delete"));

		expect(stub.gestures).toEqual(["Delete Entities"]);
		expect(stub.dispatch).toHaveBeenCalledWith("entity.destroy", { id: 1 });
		expect(stub.dispatch).toHaveBeenCalledWith("entity.destroy", { id: 2 });
		expect(stub.commit).toHaveBeenCalledTimes(1);
	});

	it("does not send a destroy for a row its parent is taking with it", () => {
		/* The engine cascades. Sending the second destroy would put a dead id
		 * in the log for no effect. */
		const stub = show({ tree: SCENE, selection: { id: 3, ids: [1, 3] } });

		fireEvent.click(screen.getByTestId("outliner-delete"));
		expect(stub.dispatch).toHaveBeenCalledWith("entity.destroy", { id: 1 });
		expect(stub.dispatch).not.toHaveBeenCalledWith("entity.destroy", { id: 3 });
	});

	it("deletes on Delete, which the tree library does not bind", () => {
		const stub = show({ tree: SCENE, selection: { id: 1, ids: [1] } });

		fireEvent.keyDown(screen.getByTestId("outliner"), { key: "Delete" });
		expect(stub.dispatch).toHaveBeenCalledWith("entity.destroy", { id: 1 });
	});

	it("leaves a modified key to the shell", () => {
		/*
		 * #949's rule, applied here: the shell owns every accelerator with a
		 * modifier and a panel owns unmodified keys while it has focus. Ctrl+Z
		 * must reach Undo even with the pointer over the tree.
		 */
		const stub = show({ tree: SCENE, selection: { id: 1, ids: [1] } });

		fireEvent.keyDown(screen.getByTestId("outliner"), {
			key: "Delete",
			ctrlKey: true,
		});
		expect(stub.dispatch).not.toHaveBeenCalled();
	});

	it("duplicates the selection as one undo step", () => {
		const stub = show({ tree: SCENE, selection: { id: 1, ids: [1] } });

		fireEvent.click(screen.getByTestId("outliner-duplicate"));
		expect(stub.gestures).toEqual(["Duplicate Entity"]);
		expect(stub.dispatch).toHaveBeenCalledWith("entity.duplicate", { id: 1 });
		expect(stub.commit).toHaveBeenCalledTimes(1);
	});

	it("offers no destructive action with nothing selected", () => {
		show({ tree: SCENE });
		expect(
			screen.getByTestId<HTMLButtonElement>("outliner-delete").disabled
		).toBe(true);
		expect(
			screen.getByTestId<HTMLButtonElement>("outliner-duplicate").disabled
		).toBe(true);
	});

	/* ---------------------------------------------------------------- *
	 * Hidden and locked
	 * ---------------------------------------------------------------- */

	it("renders hidden and locked from the engine, not from its own memory", () => {
		show({
			tree: {
				paused: false,
				entities: [node(0, -1, "root", 1), node(1, -1, "lamp", 2)],
			},
		});

		expect(
			screen.getByTestId("outliner-hide-0").getAttribute("aria-pressed")
		).toBe("true");
		expect(
			screen.getByTestId("outliner-lock-1").getAttribute("aria-pressed")
		).toBe("true");
		expect(
			screen.getByTestId("outliner-hide-1").getAttribute("aria-pressed")
		).toBe("false");
	});

	it("toggles a flag through a command, off the current value", () => {
		const { dispatch } = show({
			tree: { paused: false, entities: [node(0, -1, "root", 2)] },
		});

		fireEvent.click(screen.getByTestId("outliner-hide-0"));
		/* Locked (2) was already on and stays on — a toggle sets one bit, it
		 * does not replace the word. */
		expect(dispatch).toHaveBeenCalledWith("entity.flags", { id: 0, flags: 3 });
	});

	it("remembers flags across a mount, and replays them onto the engine", () => {
		const store = memoryStore();
		const first = stubDocument({
			tree: { paused: false, entities: [node(0, -1, "root")] },
		});
		const view = render(
			<DocumentProvider value={first.document}>
				<Outliner store={store} />
			</DocumentProvider>
		);
		fireEvent.click(screen.getByTestId("outliner-hide-0"));
		view.unmount();

		/*
		 * A fresh engine, with the flag cleared — which is what the engine
		 * really does, because the flags are not document state and it drops
		 * them when it ingests a scene. The panel is what carries the reader's
		 * view of the scene back.
		 */
		const second = stubDocument({
			tree: { paused: false, entities: [node(0, -1, "root", 0)] },
		});
		render(
			<DocumentProvider value={second.document}>
				<Outliner store={store} />
			</DocumentProvider>
		);
		expect(second.dispatch).toHaveBeenCalledWith("entity.flags", {
			id: 0,
			flags: 1,
		});
	});

	/* ---------------------------------------------------------------- *
	 * Search, and the criterion that matters
	 * ---------------------------------------------------------------- */

	it("filters without dropping the selection", () => {
		const stub = show({ tree: SCENE, selection: { id: 3, ids: [3] } });

		fireEvent.change(screen.getByTestId("outliner-search"), {
			target: { value: "lamp" },
		});

		/* Filtering is a view change and sends no command — a search that
		 * cleared the selection would lose a reader's place every keystroke. */
		expect(stub.dispatch).not.toHaveBeenCalled();
		expect(screen.queryByTestId("outliner-row-2")).toBeTruthy();
		expect(screen.queryByTestId("outliner-row-1")).toBeNull();
	});

	it("shows an entity the editor did not create, without a refresh", () => {
		/*
		 * The criterion #950 calls the valuable one. Nothing here asks for an
		 * update: a script spawned an entity, the engine's fingerprint of the
		 * world moved, the watched query re-answered, and the row is there.
		 * There is no code path in the panel that could have done this any
		 * other way, which is the point.
		 */
		const stub = show({ tree: SCENE });
		expect(screen.queryByTestId("outliner-row-9")).toBeNull();

		stub.answer({
			tree: {
				paused: false,
				entities: [...SCENE.entities, node(9, 1, "spawned")],
			},
		});

		expect(screen.getByTestId("outliner-row-9")).toBeTruthy();
		expect(screen.getByText("spawned")).toBeTruthy();
	});

	it("drops a row the engine stopped reporting", () => {
		const stub = show({ tree: SCENE });
		stub.answer({
			tree: {
				paused: false,
				entities: SCENE.entities.filter((e) => e.id !== 2),
			},
		});
		expect(screen.queryByTestId("outliner-row-2")).toBeNull();
	});
});
