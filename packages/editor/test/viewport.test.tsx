// SPDX-License-Identifier: GPL-2.0-or-later
//
// The viewport panel: the canvas that must not be re-created, and the bar that
// holds nothing.
//
// What is *not* here, on purpose: pointer capture over a real GL canvas,
// ResizeObserver actually firing, and container queries reflowing the bar.
// jsdom reports every element as zero-sized and stubs the capture API, so a
// test of any of those would pass on code a browser rejects — which is exactly
// the case #944's Q6 sent to Playwright. `e2e/viewport.spec.ts` has them.
//
// What is here is everything jsdom can answer honestly: that the element
// survives a re-render, that the bar reads the engine rather than a local
// state, and that every control dispatches a command instead of mutating.

import { fireEvent, render, screen } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";

import type { ViewportState } from "@kruddage/engine/bridge";

import { DocumentProvider } from "../src/document/document-context.js";
import type { KruddDocument } from "../src/document/document.js";
import { EngineProvider } from "../src/engine/engine-context.js";
import { GIZMO_MODE } from "../src/document/commands.js";
import { Viewport } from "../src/viewport/viewport.js";
import type { EngineInfo, EngineStatus } from "../src/engine/types.js";

function viewportState(over: Partial<ViewportState> = {}): ViewportState {
	return {
		width: 800,
		height: 600,
		editorMode: true,
		mode: GIZMO_MODE.TRANSLATE,
		axis: -1,
		dragSerial: 0,
		snap: { translate: 0, rotate: 0, scale: 0 },
		grid: { shown: true, spacing: 1 },
		...over,
	};
}

interface Stub {
	document: KruddDocument;
	dispatch: ReturnType<typeof vi.fn>;
}

function stubDocument(state: ViewportState | null): Stub {
	const dispatch = vi.fn();
	const document = {
		bridge: {
			watch: () => () => {},
			read: (kind: string) => (kind === "viewport" ? state : null),
		},
		dispatch,
		gesture: () => ({ commit: vi.fn(), abort: vi.fn() }),
		subscribe: () => () => {},
		subscribeState: () => () => {},
		state: () => ({ dirty: false, gestureDepth: 0 }),
		run: () => () => {},
		save: vi.fn(),
		load: vi.fn(),
	} as unknown as KruddDocument;
	return { document, dispatch };
}

const BUILT: EngineInfo = {
	built: true,
	base: "engine/",
	version: "0.0.0",
	exports: [],
	buildCommand: "pnpm --filter @kruddage/engine run build",
};

const READY: EngineStatus = { phase: "ready", renderer: "webgl", error: null };

function show(
	state: ViewportState | null,
	{ engine = BUILT, status = READY }: { engine?: EngineInfo; status?: EngineStatus } = {}
): Stub & { rerender: () => void } {
	const stub = stubDocument(state);
	const canvasRef = { current: null as HTMLCanvasElement | null };
	const tree = (
		<EngineProvider value={{ engine, status, log: [], canvasRef }}>
			<DocumentProvider value={stub.document}>
				<Viewport />
			</DocumentProvider>
		</EngineProvider>
	);
	const view = render(tree);
	return { ...stub, rerender: () => view.rerender(tree) };
}

describe("the canvas the engine holds", () => {
	it("carries the id the C tree looks it up by", () => {
		/*
		 * Ten call sites in the engine find this element by `#canvas` for the
		 * GL context and for every pointer callback. Renaming it costs a GL
		 * context and reports it nowhere near the cause — see boot.ts.
		 */
		show(viewportState());
		expect(screen.getByTestId("engine-canvas").id).toBe("canvas");
	});

	it("is the same element after a re-render", () => {
		/*
		 * The whole of the canvas-lifecycle criterion that jsdom can answer.
		 * React reuses a node whose position in the tree never changes, and the
		 * panel is written so that position never changes — the bar, the
		 * pointer surface and the notices are all siblings of the canvas rather
		 * than wrappers around it.
		 */
		const view = show(viewportState());
		const before = screen.getByTestId("engine-canvas");
		view.rerender();
		expect(screen.getByTestId("engine-canvas")).toBe(before);
	});

	it("survives the document arriving", () => {
		/*
		 * The real transition, and the one the sibling ordering exists for:
		 * there is no document for every frame before the wasm runtime comes
		 * up, and then there is one. The bar and the pointer surface appear
		 * around the canvas at that moment — if the canvas moved index, React
		 * would replace the node and the engine would be drawing into a
		 * detached element with no error anywhere.
		 */
		const stub = stubDocument(viewportState());
		const canvasRef = { current: null as HTMLCanvasElement | null };
		const tree = (document: KruddDocument | null) => (
			<EngineProvider
				value={{ engine: BUILT, status: READY, log: [], canvasRef }}
			>
				<DocumentProvider value={document}>
					<Viewport />
				</DocumentProvider>
			</EngineProvider>
		);

		const view = render(tree(null));
		const before = screen.getByTestId("engine-canvas");
		expect(screen.queryByTestId("viewport-bar")).toBeNull();

		view.rerender(tree(stub.document));
		expect(screen.getByTestId("viewport-bar")).toBeTruthy();
		expect(screen.getByTestId("engine-canvas")).toBe(before);
	});

	it("survives the viewport state changing under it", () => {
		const before = show(viewportState()).document;
		expect(before).toBeTruthy();
		const canvas = screen.getByTestId("engine-canvas");
		fireEvent.click(screen.getByTestId("gizmo-mode-rotate"));
		expect(screen.getByTestId("engine-canvas")).toBe(canvas);
	});

	it("is still rendered when the engine was never built", () => {
		/*
		 * The notice is a sibling, not a replacement. A panel that swapped the
		 * canvas out for the notice would have to build a second one if the
		 * engine ever arrived, and the second one is the detached element.
		 */
		show(viewportState(), { engine: { ...BUILT, built: false } });
		expect(screen.getByTestId("engine-canvas")).toBeTruthy();
		expect(screen.getByTestId("engine-unbuilt")).toBeTruthy();
	});

	it("keeps the canvas when the engine failed to start", () => {
		show(viewportState(), {
			status: { phase: "failed", renderer: null, error: "no adapter" },
		});
		expect(screen.getByTestId("engine-canvas")).toBeTruthy();
		expect(screen.getByTestId("viewport-error").textContent).toContain(
			"no adapter"
		);
	});
});

describe("the bar reads the engine and holds nothing", () => {
	it("shows the mode the engine reports, not one it was clicked into", () => {
		show(viewportState({ mode: GIZMO_MODE.ROTATE }));
		expect(
			screen.getByTestId("gizmo-mode-rotate").getAttribute("aria-checked")
		).toBe("true");
		expect(
			screen.getByTestId("gizmo-mode-move").getAttribute("aria-checked")
		).toBe("false");
	});

	it("does not move the button when clicked, only dispatches", () => {
		/*
		 * #944's Q1 applied to a toolbar. The button re-renders when the engine
		 * says the mode moved, which is what makes it agree with a mode change
		 * made by pressing R over the canvas.
		 */
		const { dispatch } = show(viewportState({ mode: GIZMO_MODE.TRANSLATE }));
		fireEvent.click(screen.getByTestId("gizmo-mode-scale"));

		expect(dispatch).toHaveBeenCalledWith("gizmo.mode", {
			mode: GIZMO_MODE.SCALE,
		});
		expect(
			screen.getByTestId("gizmo-mode-scale").getAttribute("aria-checked")
		).toBe("false");
	});

	it("edits the snap increment that belongs to the live mode", () => {
		const { dispatch } = show(
			viewportState({ mode: GIZMO_MODE.ROTATE, snap: { translate: 2, rotate: 15, scale: 0 } })
		);
		expect((screen.getByTestId("viewport-snap") as HTMLInputElement).value).toBe(
			"15"
		);

		fireEvent.change(screen.getByTestId("viewport-snap"), {
			target: { value: "45" },
		});
		expect(dispatch).toHaveBeenCalledWith("gizmo.snap", {
			translate: 2,
			rotate: 45,
			scale: 0,
		});
	});

	it("keeps a half-typed number on screen", () => {
		/*
		 * The reason the field holds its own text at all: a value that
		 * round-tripped through the engine would erase the keystroke as the
		 * reader typed it, because "0." is not the number the engine holds.
		 */
		const { dispatch } = show(viewportState());
		fireEvent.change(screen.getByTestId("viewport-snap"), {
			target: { value: "0." },
		});

		expect((screen.getByTestId("viewport-snap") as HTMLInputElement).value).toBe(
			"0."
		);
		/* It does resolve to a number — 0, which is "no snapping" — so it is
		 * sent. What must not happen is the text being rewritten. */
		expect(dispatch).toHaveBeenCalledWith("gizmo.snap", {
			translate: 0,
			rotate: 0,
			scale: 0,
		});
	});

	it("sends nothing for a cleared field", () => {
		/*
		 * `Number("")` is 0, so a field that trusted it would turn snapping off
		 * the instant a reader selected the text and pressed delete on the way
		 * to typing something else.
		 */
		const { dispatch } = show(
			viewportState({ snap: { translate: 2, rotate: 0, scale: 0 } })
		);
		fireEvent.change(screen.getByTestId("viewport-snap"), {
			target: { value: "" },
		});

		expect((screen.getByTestId("viewport-snap") as HTMLInputElement).value).toBe(
			""
		);
		expect(dispatch).not.toHaveBeenCalledWith("gizmo.snap", expect.anything());
	});

	it("sends nothing for a typo", () => {
		const { dispatch } = show(viewportState());
		fireEvent.change(screen.getByTestId("viewport-snap"), {
			target: { value: "1abc" },
		});
		expect(dispatch).not.toHaveBeenCalledWith("gizmo.snap", expect.anything());
	});

	it("refuses a negative increment rather than rounding the wrong way", () => {
		const { dispatch } = show(viewportState());
		fireEvent.change(screen.getByTestId("viewport-snap"), {
			target: { value: "-1" },
		});
		expect(dispatch).not.toHaveBeenCalledWith("gizmo.snap", expect.anything());
	});

	it("toggles the grid through a command, keeping its spacing", () => {
		const { dispatch } = show(
			viewportState({ grid: { shown: true, spacing: 4 } })
		);
		fireEvent.click(screen.getByTestId("viewport-grid"));
		expect(dispatch).toHaveBeenCalledWith("gizmo.grid", {
			shown: false,
			spacing: 4,
		});
	});
});

describe("the play/edit handover", () => {
	it("offers Play while editing and Stop while playing", () => {
		show(viewportState({ editorMode: true }));
		expect(screen.getByTestId("viewport-play").textContent).toBe("Play");
	});

	it("leaves editor mode when Play is pressed", () => {
		const { dispatch } = show(viewportState({ editorMode: true }));
		fireEvent.click(screen.getByTestId("viewport-play"));
		expect(dispatch).toHaveBeenCalledWith("engine.editing", { editing: false });
	});

	it("comes back when Stop is pressed", () => {
		const { dispatch } = show(viewportState({ editorMode: false }));
		expect(screen.getByTestId("viewport-play").textContent).toBe("Stop");
		fireEvent.click(screen.getByTestId("viewport-play"));
		expect(dispatch).toHaveBeenCalledWith("engine.editing", { editing: true });
	});

	it("does not touch the canvas either way", () => {
		/*
		 * The criterion is that entering and leaving play mode works
		 * repeatedly without a reload. It does because nothing here is torn
		 * down: the flag changes, the engine changes what it draws, and the
		 * element the GL context is on never moves.
		 */
		show(viewportState({ editorMode: true }));
		const canvas = screen.getByTestId("engine-canvas");
		for (let i = 0; i < 4; i++) {
			fireEvent.click(screen.getByTestId("viewport-play"));
			expect(screen.getByTestId("engine-canvas")).toBe(canvas);
		}
	});
});

describe("keys over the viewport", () => {
	const surface = () => screen.getByTestId("viewport-surface");

	it("switches gizmo mode on the unmodified letters", () => {
		const { dispatch } = show(viewportState());
		fireEvent.keyDown(surface(), { key: "r" });
		expect(dispatch).toHaveBeenCalledWith("gizmo.mode", {
			mode: GIZMO_MODE.ROTATE,
		});
	});

	it("frames the selection on F", () => {
		const { dispatch } = show(viewportState());
		fireEvent.keyDown(surface(), { key: "f" });
		expect(dispatch).toHaveBeenCalledWith("camera.frame", {});
	});

	it("orbits on the arrow keys", () => {
		/*
		 * The criterion is that orbit, pan and zoom work with the keyboard as
		 * well as the mouse. A viewport reachable only by dragging is one a
		 * reader who cannot drag cannot use at all.
		 */
		const { dispatch } = show(viewportState());
		fireEvent.keyDown(surface(), { key: "ArrowRight" });
		const [, payload] = dispatch.mock.calls.at(-1) ?? [];

		expect(dispatch.mock.calls.at(-1)?.[0]).toBe("camera.orbit");
		expect((payload as { dyaw: number }).dyaw).toBeLessThan(0);
	});

	it("pans on shift-arrow", () => {
		const { dispatch } = show(viewportState());
		fireEvent.keyDown(surface(), { key: "ArrowUp", shiftKey: true });
		expect(dispatch.mock.calls.at(-1)?.[0]).toBe("camera.pan");
	});

	it("zooms on plus and minus, both spellings of each", () => {
		const { dispatch } = show(viewportState());
		for (const key of ["+", "=", "-", "_"]) {
			fireEvent.keyDown(surface(), { key });
			expect(dispatch.mock.calls.at(-1)?.[0]).toBe("camera.dolly");
		}
	});

	it("returns to the authored camera on Home", () => {
		const { dispatch } = show(viewportState());
		fireEvent.keyDown(surface(), { key: "Home" });
		expect(dispatch).toHaveBeenCalledWith("camera.reset", {});
	});

	it("leaves every accelerator with a modifier to the shell", () => {
		/*
		 * The stated rule: the shell owns modified keys wherever focus is, the
		 * viewport owns unmodified letters while it has focus. Ctrl+S is Save,
		 * not Scale, even with the pointer over the scene.
		 */
		const { dispatch } = show(viewportState());
		fireEvent.keyDown(surface(), { key: "s", ctrlKey: true });
		fireEvent.keyDown(surface(), { key: "r", metaKey: true });
		expect(dispatch).not.toHaveBeenCalled();
	});

	it("lets a key it does not handle bubble", () => {
		const { dispatch } = show(viewportState());
		fireEvent.keyDown(surface(), { key: "q" });
		expect(dispatch).not.toHaveBeenCalled();
	});
});

describe("before the engine has spoken", () => {
	it("renders the canvas with no bar when there is no document", () => {
		/*
		 * There is a real window in which there is none: every frame before the
		 * wasm runtime comes up, and forever on a page whose engine was never
		 * built. The canvas has to exist for the engine to boot into.
		 */
		const canvasRef = { current: null as HTMLCanvasElement | null };
		render(
			<EngineProvider
				value={{ engine: BUILT, status: READY, log: [], canvasRef }}
			>
				<DocumentProvider value={null}>
					<Viewport />
				</DocumentProvider>
			</EngineProvider>
		);

		expect(screen.getByTestId("engine-canvas")).toBeTruthy();
		expect(screen.queryByTestId("viewport-bar")).toBeNull();
	});

	it("renders a bar with defaults when the query has no answer yet", () => {
		show(null);
		expect(screen.getByTestId("viewport-bar")).toBeTruthy();
		expect(
			screen.getByTestId("gizmo-mode-move").getAttribute("aria-checked")
		).toBe("false");
	});
});
