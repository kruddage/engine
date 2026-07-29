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

import type { ParamBlock, ParamField } from "@kruddage/engine/bridge";

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
	gesture: ReturnType<typeof vi.fn>;
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
	const gesture = vi.fn(() => ({ commit, abort: vi.fn() }));
	const document = {
		bridge: {
			watch: () => () => {},
			read: (kind: string, id = -1) => reads[`${kind}:${id}`] ?? reads[kind] ?? null,
		},
		dispatch,
		gesture,
		subscribe: () => () => {},
		subscribeState: () => () => {},
		state: () => ({ dirty: false, gestureDepth: 0 }),
		run: () => () => {},
		save: vi.fn(),
		load: vi.fn(),
	} as unknown as KruddDocument;
	return { document, dispatch, gesture, commit };
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

		/*
		 * Labelled, and holding the value — the entry is the control a reader
		 * reaches by name, whether or not the declaration also earned a slider.
		 */
		const entry = screen.getByLabelText("wobbliness") as HTMLInputElement;
		expect(entry.value).toBe("2.5");

		/* And the authored bounds reached the slider beside it. */
		const slider = screen.getByTestId("param-input-wobbliness-0") as HTMLInputElement;
		expect(slider.type).toBe("range");
		expect(slider.min).toBe("0");
		expect(slider.max).toBe("10");
		expect(slider.value).toBe("2.5");

		/*
		 * And it edits — through a command, addressed by slot and field index
		 * rather than by name, because the engine's declaration is what those
		 * indices refer to.
		 */
		fireEvent.change(slider, { target: { value: "7" } });
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
		const control = screen.getByTestId("param-input-width-0");

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

/* ------------------------------------------------------------------ *
 * Refusing a value, and saying why
 * ------------------------------------------------------------------ */

/** One entity, one mesh block, whatever fields the case needs. */
function withFields(fields: ParamField[], overrides: Partial<ParamBlock> = {}): Stub {
	const stub = show({
		selection: { id: 3, ids: [3] },
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
					fields,
					...overrides,
				},
			],
		},
	});
	fireEvent.mouseDown(screen.getByRole("tab", { name: "Mesh" }));
	return stub;
}

describe("a value the control cannot take", () => {
	it("says what is wrong with text that is not a number, and writes nothing", () => {
		const { dispatch } = withFields([field()]);
		fireEvent.change(screen.getByLabelText("width"), { target: { value: "wide" } });

		expect(screen.getByText("not a number")).toBeTruthy();
		expect(screen.getByLabelText("width").getAttribute("aria-invalid")).toBe("true");
		expect(dispatch).not.toHaveBeenCalled();
	});

	it("ties the reason to the control, so it is not only a colour", () => {
		withFields([field()]);
		const input = screen.getByLabelText("width");
		fireEvent.change(input, { target: { value: "wide" } });

		const described = input.getAttribute("aria-describedby");
		expect(described).toBeTruthy();
		expect(globalThis.document.getElementById(described ?? "")?.textContent).toBe(
			"not a number"
		);
	});

	it("says a typed value was outside the declared range, and clamps it", () => {
		const { dispatch } = withFields([
			field({ edit: "range", min: 0, max: 1, value: [0.5] }),
		]);
		fireEvent.change(screen.getByLabelText("width"), { target: { value: "5" } });

		expect(screen.getByText("outside 0–1, clamped")).toBeTruthy();
		expect(dispatch).toHaveBeenCalledWith("entity.param", {
			id: 3,
			slot: 0,
			field: 0,
			value: [1],
		});
	});

	it("says so when the engine's own value is out of range, rather than showing the bound", () => {
		/*
		 * A slider cannot render a value outside its bounds — the thumb sits
		 * at the bound whatever we do. An older project holding 5 for a 0..1
		 * parameter must not be reported as holding 1.
		 */
		withFields([field({ edit: "range", min: 0, max: 1, value: [5] })]);

		expect((screen.getByLabelText("width") as HTMLInputElement).value).toBe("5");
		expect(screen.getByText("outside 0–1")).toBeTruthy();
	});

	it("says a fraction typed into an int was rounded", () => {
		const { dispatch } = withFields([field({ type: "int", value: [2] })]);
		fireEvent.change(screen.getByLabelText("width"), { target: { value: "3.7" } });

		expect(screen.getByText("rounded to a whole number")).toBeTruthy();
		expect(dispatch).toHaveBeenCalledWith("entity.param", {
			id: 3,
			slot: 0,
			field: 0,
			value: [4],
		});
	});
});

/* ------------------------------------------------------------------ *
 * Drag-to-scrub
 * ------------------------------------------------------------------ */

describe("dragging a number field", () => {
	it("scrubs the value, as one undo step", () => {
		const { dispatch, gesture, commit } = withFields([field({ value: [1] })]);
		const input = screen.getByLabelText("width");

		fireEvent.pointerDown(input, { pointerId: 1, clientX: 100 });
		fireEvent.pointerMove(input, { pointerId: 1, clientX: 150 });
		fireEvent.pointerMove(input, { pointerId: 1, clientX: 200 });

		/* 0.01 per pixel, from where the press started — not from wherever the
		 * last move left it, which would drift over a long drag. */
		expect(dispatch).toHaveBeenLastCalledWith("entity.param", {
			id: 3,
			slot: 0,
			field: 0,
			value: [2],
		});
		expect(gesture).toHaveBeenCalledTimes(1);
		expect(commit).not.toHaveBeenCalled();

		fireEvent.pointerUp(input, { pointerId: 1, clientX: 200 });
		expect(commit).toHaveBeenCalledTimes(1);
	});

	it("leaves a press that does not travel as a click into the field", () => {
		const { dispatch } = withFields([field({ value: [1] })]);
		const input = screen.getByLabelText("width");

		fireEvent.pointerDown(input, { pointerId: 1, clientX: 100 });
		fireEvent.pointerMove(input, { pointerId: 1, clientX: 101 });
		fireEvent.pointerUp(input, { pointerId: 1, clientX: 101 });

		expect(dispatch).not.toHaveBeenCalled();
		expect(globalThis.document.activeElement).toBe(input);
	});

	it("leaves a focused field alone, so text can still be selected by dragging", () => {
		const { dispatch } = withFields([field({ value: [1] })]);
		const input = screen.getByLabelText("width");

		fireEvent.focus(input);
		fireEvent.pointerDown(input, { pointerId: 1, clientX: 100 });
		fireEvent.pointerMove(input, { pointerId: 1, clientX: 200 });

		expect(dispatch).not.toHaveBeenCalled();
	});

	it("does not scrub on touch, where the same gesture scrolls the panel", () => {
		const { dispatch } = withFields([field({ value: [1] })]);
		const input = screen.getByLabelText("width");

		fireEvent.pointerDown(input, { pointerId: 1, clientX: 100, pointerType: "touch" });
		fireEvent.pointerMove(input, { pointerId: 1, clientX: 200, pointerType: "touch" });

		expect(dispatch).not.toHaveBeenCalled();
	});
});

/* ------------------------------------------------------------------ *
 * More than one entity
 * ------------------------------------------------------------------ */

/**
 * A mesh block, with the fields given in the order given.
 *
 * The order is the point of the parameter: two entities carrying different
 * meshes declare the same parameter at different indices, and a merge by index
 * would edit the wrong field on the second one.
 */
function meshBlock(fields: ParamField[]): ParamBlock {
	return {
		slot: "mesh",
		asset: 42,
		path: "m",
		size: 8,
		overridden: true,
		truncated: false,
		fields,
	};
}

function showPair(
	first: ParamBlock[],
	second: ParamBlock[],
	ids: number[] = [3, 4]
): Stub {
	const stub = show({
		selection: { id: 3, ids },
		"entity:3": ENTITY,
		"entity:4": { ...ENTITY, id: 4, name: "crate.001" },
		"entity.params:3": { id: 3, blocks: first },
		"entity.params:4": { id: 4, blocks: second },
	});
	return stub;
}

describe("a selection of several entities", () => {
	it("says how many are selected and whose facts it is showing", () => {
		showPair([meshBlock([field()])], [meshBlock([field()])]);
		expect(screen.getByTestId("inspector-count").textContent).toMatch(
			/2 entities selected/
		);
	});

	it("edits a shared parameter on all of them, at each one's own index", () => {
		const { dispatch, gesture, commit } = showPair(
			[meshBlock([field({ name: "width" }), field({ name: "depth" })])],
			/* Same two parameters, declared the other way round. */
			[meshBlock([field({ name: "depth" }), field({ name: "width" })])]
		);
		fireEvent.mouseDown(screen.getByRole("tab", { name: "Mesh" }));

		fireEvent.change(screen.getByLabelText("width"), { target: { value: "5" } });

		expect(dispatch).toHaveBeenCalledWith("entity.param", {
			id: 3,
			slot: 0,
			field: 0,
			value: [5],
		});
		expect(dispatch).toHaveBeenCalledWith("entity.param", {
			id: 4,
			slot: 0,
			/* Not 0. Merging by index would have written `depth` here. */
			field: 1,
			value: [5],
		});

		/* One gesture around both writes, so it is one undo step. */
		expect(gesture).toHaveBeenCalledTimes(1);
		fireEvent.blur(screen.getByLabelText("width"));
		expect(commit).toHaveBeenCalledTimes(1);
	});

	it("shows a differing value as differing, not as the first one", () => {
		showPair(
			[meshBlock([field({ name: "width", value: [1] })])],
			[meshBlock([field({ name: "width", value: [2] })])]
		);
		fireEvent.mouseDown(screen.getByRole("tab", { name: "Mesh" }));

		const input = screen.getByLabelText("width") as HTMLInputElement;
		expect(input.value).toBe("");
		expect(input.placeholder).toBe("—");
		expect(screen.getByText("mixed")).toBeTruthy();
	});

	it("hides a parameter the whole selection does not share, and says it did", () => {
		showPair(
			[meshBlock([field({ name: "width" }), field({ name: "wobbliness" })])],
			[meshBlock([field({ name: "width" })])]
		);
		fireEvent.mouseDown(screen.getByRole("tab", { name: "Mesh" }));

		expect(screen.queryByLabelText("wobbliness")).toBeNull();
		expect(screen.getByText(/1 parameter is not shared/)).toBeTruthy();
	});

	it("drops a whole slot only one of them carries", () => {
		showPair(
			[
				meshBlock([field()]),
				{ ...meshBlock([field()]), slot: "script", asset: 9, path: "s" },
			],
			[meshBlock([field()])]
		);
		/* A Script tab whose every edit reached one of the two would be worse
		 * than no tab, because it would look like it reached both. */
		expect(screen.queryByRole("tab", { name: "Script" })).toBeNull();
		expect(screen.getByRole("tab", { name: "Mesh" })).toBeTruthy();
	});

	/*
	 * THE criterion again, with the selection that came after it. A parameter
	 * no source file in this package mentions has to reach a working control
	 * across a multi-selection too — otherwise "zero editor code" would hold
	 * for one entity and quietly stop holding for two.
	 */
	it("builds a control for a parameter it has never heard of across a selection", () => {
		const { dispatch } = showPair(
			[meshBlock([field({ name: "wobbliness", edit: "range", min: 0, max: 10, value: [2] })])],
			[meshBlock([field({ name: "wobbliness", edit: "range", min: 0, max: 10, value: [8] })])]
		);
		fireEvent.mouseDown(screen.getByRole("tab", { name: "Mesh" }));

		const slider = screen.getByTestId("param-input-wobbliness-0") as HTMLInputElement;
		expect(slider.max).toBe("10");
		expect(screen.getByText("mixed")).toBeTruthy();

		fireEvent.change(slider, { target: { value: "7" } });
		expect(dispatch).toHaveBeenCalledTimes(2);
		expect(dispatch).toHaveBeenCalledWith("entity.param", {
			id: 4,
			slot: 0,
			field: 0,
			value: [7],
		});
	});

	it("waits out an entity the engine has not answered for", () => {
		show({
			selection: { id: 3, ids: [3, 4] },
			"entity:3": ENTITY,
			"entity.params:3": { id: 3, blocks: [meshBlock([field()])] },
			/* No `entity.params:4` — the query was asked this frame. */
		});
		expect(screen.getByText(/has not answered yet/)).toBeTruthy();
	});
});
