// SPDX-License-Identifier: GPL-2.0-or-later
//
// The shell, rendered.
//
// What is asserted here is the frame's behaviour — where panels land, that
// there is only ever one of each, that hiding and moving and resetting work,
// and that an unwired action says so. What is deliberately *not* asserted is
// anything about size: jsdom reports every element as zero pixels, so a test
// about a dragged splitter would be a test about jsdom. That is Playwright's,
// and #944's Q6 rule is the reason it is not here.

import { describe, expect, it, vi } from "vitest";
import { act, fireEvent, render, screen, within } from "@testing-library/react";
import type { RenderResult } from "@testing-library/react";

import { EngineProvider } from "../src/engine/engine-context.js";
import type { EngineInfo, EngineStatus } from "../src/engine/types.js";
import { HINT_MS, useShell, type Shell as ShellState } from "../src/shell/use-shell.js";
import { memoryStore, type LayoutStore } from "../src/shell/layout.js";
import { panels, registerPanel } from "../src/shell/panels.js";
import { registerBuiltinPanels } from "../src/panels/index.js";
import { Shell } from "../src/shell/shell.js";

const ENGINE: EngineInfo = {
	built: true,
	version: "19.3.0",
	exports: ["_main"],
	base: "engine/",
	buildCommand: "pnpm --filter @kruddage/engine run build",
};

const READY: EngineStatus = { phase: "ready", renderer: "webgpu", error: null };

/**
 * Mount the shell over a store the test owns.
 *
 * useShell is called from a harness component rather than a hook-testing
 * helper so the shell is driven exactly as the app drives it — the interesting
 * failures here are about the frame reacting to state, not about the hook.
 */
function mount(
	store: LayoutStore = memoryStore(),
	status: EngineStatus = READY
): RenderResult & { store: LayoutStore; shell: () => ShellState } {
	/*
	 * The live shell object is captured so a test can drive an action the way
	 * a menu item does. Radix's menus open on pointer events that jsdom
	 * reports without layout, and a test that fought that would be a test about
	 * jsdom — the menu's own rendering is asserted separately, and the action
	 * path is asserted here.
	 */
	let live: ShellState | null = null;

	function Harness(): React.JSX.Element {
		const definitions = panels();
		const shell = useShell(definitions, store);
		live = shell;
		return (
			<EngineProvider
				value={{
					engine: ENGINE,
					status,
					log: [],
					canvasRef: { current: null },
				}}
			>
				<Shell
					shell={shell}
					panels={definitions}
					engine={ENGINE}
					status={status}
					stats={{ fps: 60, resolution: { width: 1280, height: 720 } }}
				/>
			</EngineProvider>
		);
	}

	const result = render(<Harness />);
	return {
		...result,
		store,
		shell: () => {
			if (live === null) throw new Error("the shell has not rendered");
			return live;
		},
	};
}

function withBuiltins(): void {
	registerBuiltinPanels();
}

describe("the frame", () => {
	it("renders the chrome in the vocabulary's order", () => {
		withBuiltins();
		mount();

		expect(screen.getByRole("menubar", { name: "Command bar" })).toBeTruthy();
		expect(screen.getByRole("toolbar", { name: "Toolbar" })).toBeTruthy();
		expect(screen.getByTestId("rail")).toBeTruthy();
		expect(screen.getByTestId("viewport-deck")).toBeTruthy();
		expect(screen.getByTestId("status-strip")).toBeTruthy();
	});

	it("places each panel at its home slot", () => {
		withBuiltins();
		mount();

		const tab = (slot: string, name: string): HTMLElement =>
			within(screen.getByTestId(`dock-${slot}`)).getByRole("tab", { name });

		expect(tab("left", "Outliner")).toBeTruthy();
		expect(tab("right", "Inspector")).toBeTruthy();
		expect(tab("bottom", "Assets")).toBeTruthy();
		expect(tab("bottom", "Console")).toBeTruthy();
		expect(tab("bottom", "Build")).toBeTruthy();
	});

	it("constructs exactly one instance of a panel", () => {
		/*
		 * Mount, don't duplicate. #886's mockup grew three Inspectors that
		 * drifted apart; this is the assertion that makes that impossible
		 * rather than merely discouraged.
		 */
		withBuiltins();
		mount();

		expect(screen.getAllByTestId("engine-canvas")).toHaveLength(1);
		/* The inspector renders its waiting state here — this suite mounts the
		 * shell without a document — and there is exactly one of that too. */
		expect(screen.getAllByTestId("inspector-waiting")).toHaveLength(1);
	});

	it("renders a panel added to the registry without any edit to the shell", () => {
		/*
		 * The criterion #950, #951 and #952 all depend on. If this needs a
		 * change in shell.tsx, each of those PRs touches the frame.
		 */
		withBuiltins();
		registerPanel({
			id: "assets",
			title: "Later Panel",
			home: "left",
			render: () => <p data-testid="later-panel">a panel from elsewhere</p>,
		});
		mount();

		expect(
			within(screen.getByTestId("dock-left")).getByRole("tab", {
				name: "Later Panel",
			})
		).toBeTruthy();
	});

	it("renders no dock at all for a slot with everything hidden", () => {
		withBuiltins();
		mount();

		fireEvent.click(screen.getByTestId("rail-outliner"));
		/* A tab strip over an empty frame, with a splitter that resizes
		 * nothing, is worse than the frame not being there. */
		expect(screen.queryByTestId("dock-left")).toBeNull();
	});
});

describe("panels", () => {
	it("shows and hides from the rail", () => {
		withBuiltins();
		mount();

		const button = screen.getByTestId("rail-assets");
		expect(button.getAttribute("aria-pressed")).toBe("true");

		fireEvent.click(button);
		expect(button.getAttribute("aria-pressed")).toBe("false");
		expect(
			within(screen.getByTestId("dock-bottom")).queryByRole("tab", {
				name: "Assets",
			})
		).toBeNull();
	});

	it("offers no rail button for a panel that may not be hidden", () => {
		withBuiltins();
		mount();

		expect(screen.queryByTestId("rail-viewport")).toBeNull();
	});

	it("raises a tab when it is picked", () => {
		withBuiltins();
		mount();

		const console_ = screen.getByTestId("tab-console");
		expect(console_.getAttribute("data-state")).toBe("inactive");

		/* Radix Tabs selects on mousedown, not on click — a tab strip that only
		 * responded to a full click would feel a beat slow. */
		fireEvent.mouseDown(console_);
		expect(console_.getAttribute("data-state")).toBe("active");
		expect(screen.getByTestId("console")).toBeTruthy();
	});

	it("says plainly that an unimplemented panel is not built yet", () => {
		withBuiltins();
		mount();

		/*
		 * The assets panel, since #950 gave the outliner real contents. The
		 * assertion is about the *placeholder*, not about which panel is one —
		 * so it follows whichever is still empty rather than being deleted with
		 * the panel it happened to name.
		 */
		const bottom = within(screen.getByTestId("dock-bottom"));
		expect(bottom.getByText("Asset Browser")).toBeTruthy();
		expect(bottom.getByText(/Not built yet/)).toBeTruthy();
	});

	it("mounts the outliner rather than a placeholder", () => {
		withBuiltins();
		mount();

		/*
		 * This shell suite renders without a document, so what the outliner
		 * shows here is its waiting state — which is the point: the panel is
		 * real, it is placed at its home dock, and it says what it knows
		 * instead of rendering an empty box. What it does with a document is
		 * test/outliner.test.tsx's.
		 */
		const left = within(screen.getByTestId("dock-left"));
		expect(left.getByTestId("outliner-waiting")).toBeTruthy();
	});

	it("gives the inspector one dock rather than three", () => {
		withBuiltins();
		mount();

		/*
		 * The tab group itself moved into the inspector's own suite when #951
		 * made its tabs derive from the selection: it needs a document to have
		 * any, and this suite deliberately renders the shell without one. What
		 * the shell still owes is that there is one inspector dock and not a
		 * column of them, which is the layout claim this test was making.
		 */
		expect(screen.getByTestId("inspector-waiting")).toBeTruthy();
		expect(screen.queryByTestId("dock-far-right")).toBeNull();
	});
});

describe("layout persistence", () => {
	it("remembers a hidden panel across a remount", () => {
		withBuiltins();
		const store = memoryStore();

		const first = mount(store);
		fireEvent.click(screen.getByTestId("rail-console"));
		first.unmount();

		mount(store);
		expect(screen.getByTestId("rail-console").getAttribute("aria-pressed")).toBe(
			"false"
		);
	});

	it("remembers which tab a slot has raised", () => {
		withBuiltins();
		const store = memoryStore();

		const first = mount(store);
		fireEvent.mouseDown(screen.getByTestId("tab-console"));
		first.unmount();

		mount(store);
		expect(screen.getByTestId("tab-console").getAttribute("data-state")).toBe(
			"active"
		);
	});

	it("puts everything back on Reset Layout, and forgets the stored one", () => {
		withBuiltins();
		const store = memoryStore();
		const shell = mount(store);

		fireEvent.click(screen.getByTestId("rail-console"));
		fireEvent.click(screen.getByTestId("rail-assets"));
		expect(screen.getByTestId("rail-console").getAttribute("aria-pressed")).toBe(
			"false"
		);

		act(() => {
			shell.shell().run("reset-layout", "Reset &Layout");
		});

		expect(screen.getByTestId("rail-console").getAttribute("aria-pressed")).toBe(
			"true"
		);
		expect(screen.getByTestId("status-hint").textContent).toBe("layout reset");
		/*
		 * And the store is cleared, not overwritten — a reset a reload undoes
		 * is the only way a reset can be worse than doing nothing.
		 */
		shell.unmount();
		mount(store);
		expect(screen.getByTestId("rail-console").getAttribute("aria-pressed")).toBe(
			"true"
		);
	});

	it("moves a panel to another slot, and keeps exactly one of it", () => {
		withBuiltins();
		const shell = mount();

		act(() => {
			/* The path View > Move > Console > Left drives. */
			shell.shell().movePanel("console", "left");
		});

		const left = within(screen.getByTestId("dock-left"));
		expect(left.getByRole("tab", { name: "Console" })).toBeTruthy();
		expect(
			within(screen.getByTestId("dock-bottom")).queryByRole("tab", {
				name: "Console",
			})
		).toBeNull();
		/* Mounted once and moved — not constructed a second time. */
		expect(screen.getAllByTestId("tab-console")).toHaveLength(1);
	});

	it("shows a panel that a move sends somewhere it was hidden from", () => {
		withBuiltins();
		const shell = mount();

		fireEvent.click(screen.getByTestId("rail-console"));
		act(() => {
			shell.shell().movePanel("console", "left");
		});

		/* Sending something to a slot and having nothing appear reads as a
		 * broken menu, not as a hidden panel. */
		expect(
			within(screen.getByTestId("dock-left")).getByRole("tab", {
				name: "Console",
			})
		).toBeTruthy();
	});
});

describe("the status strip", () => {
	it("shows fps, resolution and driver with the by-id contract intact", () => {
		withBuiltins();
		mount();

		expect(screen.getByTestId("status-fps").textContent).toBe("60 fps");
		expect(screen.getByTestId("status-resolution").textContent).toBe("1280×720");
		expect(screen.getByTestId("status-driver").textContent).toBe("WebGPU");
		/* The element ids `editor_layout.scm` declares. A host updating a field
		 * by id still finds it. */
		expect(document.getElementById("status-fps")).toBeTruthy();
		expect(document.getElementById("status-resolution")).toBeTruthy();
		expect(document.getElementById("status-driver")).toBeTruthy();
	});

	it("shows the renderer badge, and says why a failed backend failed", () => {
		withBuiltins();
		mount(memoryStore(), {
			phase: "failed",
			renderer: "webgpu",
			error: "this browser returned no GPU adapter",
		});

		const badge = screen.getByTestId("badge-renderer");
		expect(badge.textContent).toContain("WebGPU");
		expect(badge.textContent).toContain("unavailable");
		expect(badge.getAttribute("title")).toBe(
			"this browser returned no GPU adapter"
		);
	});

	it("shows a transient hint for an unwired action and then clears it", () => {
		vi.useFakeTimers();
		withBuiltins();
		mount();

		act(() => {
			fireEvent.keyDown(window, { key: "s", ctrlKey: true });
		});
		const hint = screen.getByTestId("status-hint");
		expect(hint.textContent).toBe("Save: coming soon");

		act(() => {
			vi.advanceTimersByTime(HINT_MS + 1);
		});
		expect(hint.textContent).toBe("");
	});
});

describe("keyboard shortcuts", () => {
	it("swallows a chord it owns", () => {
		withBuiltins();
		mount();

		const event = new KeyboardEvent("keydown", {
			key: "s",
			ctrlKey: true,
			cancelable: true,
			bubbles: true,
		});
		act(() => {
			window.dispatchEvent(event);
		});

		/* Otherwise Ctrl+S opens the browser's save dialog over an editor that
		 * has its own Save. */
		expect(event.defaultPrevented).toBe(true);
	});

	it("leaves a chord alone while a text field has focus", () => {
		withBuiltins();
		mount();

		const field = document.createElement("input");
		document.body.appendChild(field);
		field.focus();

		const event = new KeyboardEvent("keydown", {
			key: "c",
			ctrlKey: true,
			cancelable: true,
			bubbles: true,
		});
		act(() => {
			field.dispatchEvent(event);
		});

		/* Ctrl+C in a rename field is a copy. An editor that intercepts it is
		 * one that cannot be typed in. */
		expect(event.defaultPrevented).toBe(false);
		expect(screen.getByTestId("status-hint").textContent).toBe("");
		field.remove();
	});
});
