// SPDX-License-Identifier: GPL-2.0-or-later
//
// The inspector, and the one property it exists to have.
//
// #951's criterion — "adding a parameter with an `(edit)` clause produces a
// working control with zero editor code" — is the last test in this file, and
// it is written to be checkable rather than asserted: it introduces a parameter
// this suite has never seen, under a name nothing in `src/` mentions, and
// requires a working control to appear.

import { fireEvent, render, screen, within } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import type { ParamField } from "@kruddage/engine/bridge";

import { constrain, controlFor, fromHex, toHex } from "../src/panels/inspector/controls.js";
import { Inspector } from "../src/panels/inspector/inspector.js";
import { DocumentProvider } from "../src/document/document-context.js";
import type { KruddDocument } from "../src/document/document.js";

function field(overrides: Partial<ParamField> = {}): ParamField {
	return {
		name: "width",
		type: "float",
		components: 1,
		edit: "none",
		min: 0,
		max: 0,
		value: [1],
		...overrides,
	};
}

describe("deriving a control from a declaration", () => {
	it("gives a range field a slider with its authored bounds", () => {
		const spec = controlFor(field({ edit: "range", min: 0.5, max: 4 }));
		expect(spec).toMatchObject({ kind: "slider", min: 0.5, max: 4 });
	});

	it("gives a colour hint a colour well, not three numbers", () => {
		/*
		 * The hint is consulted before the type. A file that checked the type
		 * first would render this as a vector and silently ignore every hint on
		 * a multi-component field.
		 */
		const spec = controlFor(field({ type: "vec3", components: 3, edit: "color" }));
		expect(spec.kind).toBe("color");
	});

	it("gives a field with no hint a plain number rather than nothing", () => {
		expect(controlFor(field()).kind).toBe("number");
	});

	it("gives a multi-component field with no hint a vector", () => {
		expect(controlFor(field({ type: "vec2", components: 2 })).kind).toBe("vector");
	});

	it("falls back when a declared range could not build a slider", () => {
		/* A zero-width range is a slider that cannot move — worse than a box. */
		expect(controlFor(field({ edit: "range", min: 1, max: 1 })).kind).toBe("number");
		expect(
			controlFor(field({ edit: "range", min: 0, max: Number.NaN })).kind
		).toBe("number");
	});

	it("keeps an int integral through whatever control it gets", () => {
		const spec = controlFor(field({ type: "int", edit: "range", min: 0, max: 10 }));
		expect(spec.kind).toBe("slider");
		expect(constrain(spec, 3.7)).toBe(4);
	});

	it("clamps to the declared range on the way out", () => {
		const spec = controlFor(field({ edit: "range", min: 0, max: 1 }));
		expect(constrain(spec, 5)).toBe(1);
		expect(constrain(spec, -5)).toBe(0);
	});

	it("does not clamp a field with no declared range", () => {
		expect(constrain(controlFor(field()), 5000)).toBe(5000);
	});

	it("round-trips a colour through hex", () => {
		expect(toHex([1, 0, 0])).toBe("#ff0000");
		expect(fromHex("#00ff00")).toEqual([0, 1, 0]);
		expect(fromHex("not a colour")).toBeNull();
	});
});

/* ------------------------------------------------------------------ *
 * The panel
 * ------------------------------------------------------------------ */

interface Stub {
	document: KruddDocument;
	dispatch: ReturnType<typeof vi.fn>;
	commit: ReturnType<typeof vi.fn>;
}

/**
 * A document whose reads are canned and whose writes are recorded.
 *
 * The bridge itself is exercised by the engine package's suite and by
 * bridge_test.c; what is under test here is the panel — that it asks for the
 * right things and dispatches the right commands.
 */
function stubDocument(reads: Record<string, unknown>): Stub {
	const dispatch = vi.fn();
	const commit = vi.fn();
	const document = {
		bridge: {
			watch: () => () => {},
			read: (kind: string, id = -1) => reads[`${kind}:${id}`] ?? reads[kind] ?? null,
		},
		dispatch,
		gesture: () => ({ commit, abort: vi.fn() }),
		subscribe: () => () => {},
		subscribeState: () => () => {},
		state: () => ({ dirty: false, gestureDepth: 0 }),
		run: () => () => {},
		save: vi.fn(),
		load: vi.fn(),
	} as unknown as KruddDocument;
	return { document, dispatch, commit };
}

function show(reads: Record<string, unknown>): Stub {
	const stub = stubDocument(reads);
	render(
		<DocumentProvider value={stub.document}>
			<Inspector />
		</DocumentProvider>
	);
	return stub;
}

const ENTITY = {
	id: 3,
	parent: -1,
	name: "crate",
	mask: 2,
	render: 42,
	material: 0,
	script: 0,
	local: { position: [1, 2, 3], rotation: [0, 0, 0, 1], scale: [1, 1, 1] },
	world: { position: [1, 2, 3], rotation: [0, 0, 0, 1], scale: [1, 1, 1] },
};

describe("the inspector panel", () => {
	it("says so plainly when nothing is selected", () => {
		show({ selection: { id: -1 } });
		expect(screen.getByTestId("inspector-empty")).toBeTruthy();
		expect(screen.queryByTestId("inspector")).toBeNull();
	});

	it("shows the entity's own facts on the entity tab", () => {
		show({ selection: { id: 3 }, "entity:3": ENTITY });
		const facts = screen.getByTestId("inspector-facts");
		expect(within(facts).getByText("crate")).toBeTruthy();
		expect(within(facts).getByText("root")).toBeTruthy();
	});

	it("is one panel with a tab per block, not a dock per block", () => {
		show({
			selection: { id: 3 },
			"entity:3": ENTITY,
			"entity.params:3": {
				id: 3,
				blocks: [
					{ slot: "mesh", asset: 42, path: "m", size: 4, overridden: false, truncated: false, fields: [] },
					{ slot: "script", asset: 9, path: "s", size: 4, overridden: false, truncated: false, fields: [] },
				],
			},
		});
		/* One instance. Three tabs inside it — the entity's, plus one per block. */
		expect(screen.getAllByTestId("inspector")).toHaveLength(1);
		expect(screen.getAllByRole("tab")).toHaveLength(3);
	});

	it("says an asset declares no parameters rather than showing an empty tab", () => {
		show({
			selection: { id: 3 },
			"entity:3": ENTITY,
			"entity.params:3": {
				id: 3,
				blocks: [
					{
						slot: "mesh",
						asset: 42,
						path: "builtin://mesh/plain",
						size: 0,
						overridden: false,
						truncated: false,
						fields: [],
					},
				],
			},
		});
		fireEvent.mouseDown(screen.getByRole("tab", { name: "Mesh" }));
		expect(screen.getByText(/declares no parameters/)).toBeTruthy();
	});

	/*
	 * THE criterion. `wobbliness` appears in no source file in this package —
	 * grep for it and this test is the only hit. It arrives with an authored
	 * `(edit range)` hint and must become a working, labelled, bounded control
	 * with no code written for it.
	 */
	it("builds a working control for a parameter it has never heard of", () => {
		const { dispatch } = show({
			selection: { id: 3 },
			"entity:3": ENTITY,
			"entity.params:3": {
				id: 3,
				blocks: [
					{
						slot: "mesh",
						asset: 42,
						path: "builtin://mesh/probe",
						size: 4,
						overridden: false,
						truncated: false,
						fields: [
							field({
								name: "wobbliness",
								edit: "range",
								min: 0,
								max: 10,
								value: [2.5],
							}),
						],
					},
				],
			},
		});

		fireEvent.mouseDown(screen.getByRole("tab", { name: "Mesh" }));

		const control = screen.getByLabelText("wobbliness") as HTMLInputElement;
		expect(control.type).toBe("range");
		expect(control.min).toBe("0");
		expect(control.max).toBe("10");
		expect(control.value).toBe("2.5");

		/*
		 * And it edits — through a command, addressed by slot and field index
		 * rather than by name, because the engine's declaration is what those
		 * indices refer to.
		 */
		fireEvent.change(control, { target: { value: "7" } });
		expect(dispatch).toHaveBeenCalledWith("entity.param", {
			id: 3,
			slot: 0,
			field: 0,
			value: [7],
		});
	});

	it("brackets a drag in a gesture, so it is one undo step", () => {
		const { commit } = show({
			selection: { id: 3 },
			"entity:3": ENTITY,
			"entity.params:3": {
				id: 3,
				blocks: [
					{
						slot: "mesh",
						asset: 42,
						path: "m",
						size: 4,
						overridden: true,
						truncated: false,
						fields: [field({ edit: "range", min: 0, max: 10, value: [1] })],
					},
				],
			},
		});

		fireEvent.mouseDown(screen.getByRole("tab", { name: "Mesh" }));
		const control = screen.getByLabelText("width");

		/*
		 * Down, several moves, up — one gesture, closed once. The engine
		 * coalesces the commands inside it into a single history entry, which
		 * is the same thing a C-side gizmo drag gets (#944, Q2).
		 */
		fireEvent.pointerDown(control);
		fireEvent.change(control, { target: { value: "2" } });
		fireEvent.change(control, { target: { value: "3" } });
		expect(commit).not.toHaveBeenCalled();
		fireEvent.pointerUp(control);
		expect(commit).toHaveBeenCalledTimes(1);
	});

	it("warns when a block carries more parameters than the boundary does", () => {
		show({
			selection: { id: 3 },
			"entity:3": ENTITY,
			"entity.params:3": {
				id: 3,
				blocks: [
					{
						slot: "mesh",
						asset: 42,
						path: "m",
						size: 4,
						overridden: false,
						truncated: true,
						fields: [field()],
					},
				],
			},
		});
		/*
		 * Never silent: a truncated block would otherwise look exactly like an
		 * asset that has fewer parameters than it does.
		 */
		fireEvent.mouseDown(screen.getByRole("tab", { name: "Mesh" }));
		expect(screen.getByText(/more parameters than the editor can show/)).toBeTruthy();
	});
});
