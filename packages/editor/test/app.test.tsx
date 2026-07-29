// SPDX-License-Identifier: GPL-2.0-or-later
//
// The application: the engine's two states, and the shell over both of them.
//
// The unbuilt branch is the one worth a test. It is the case a contributor
// without emsdk hits on their first `pnpm dev`, it is the case CI's workspace
// job runs in, and it is the one where rendering an empty box instead of an
// explanation would be indistinguishable from the editor being broken.
//
// The shell's own behaviour is test/shell.test.tsx's. What is asserted here is
// only what App is responsible for: registering the panels, booting the engine,
// and handing both to the frame.

import { beforeEach, describe, expect, it, vi } from "vitest";
import { render, screen } from "@testing-library/react";

import { registerBuiltinPanels } from "../src/panels/index.js";

import type { EngineInfo } from "../src/engine/types.js";

const BUILT: EngineInfo = {
	built: true,
	version: "19.3.0",
	exports: ["_main", "_krudd_load_game"],
	base: "engine/",
	buildCommand: "pnpm --filter @kruddage/engine run build",
};

const UNBUILT: EngineInfo = {
	built: false,
	version: null,
	exports: [],
	base: "engine/",
	buildCommand: "pnpm --filter @kruddage/engine run build",
};

/* Mutable so each test can choose which engine the module under test sees. The
 * mock factory is hoisted above the imports, so it may not close over a `let`
 * declared here — hence the object, whose property is read at call time. */
const current: { engine: EngineInfo } = { engine: BUILT };

vi.mock("virtual:krudd-engine", () => ({
	get engine() {
		return current.engine;
	},
}));

const { App } = await import("../src/app.js");

/*
 * App registers the panels at module scope, which runs once — and the suite's
 * afterEach clears the registry so one test's panels cannot leak into the next.
 * Re-registering here is what reconciles the two. registerPanel is keyed by id,
 * so calling it again is idempotent rather than a second set of panels.
 */
beforeEach(() => {
	registerBuiltinPanels();
});

describe("App", () => {
	it("renders the shell with every panel registered", () => {
		current.engine = BUILT;
		render(<App />);

		expect(screen.getByTestId("shell")).toBeTruthy();
		for (const slot of ["left", "right", "bottom"]) {
			expect(screen.getByTestId(`dock-${slot}`)).toBeTruthy();
		}
	});

	it("shows the engine's version in the toolbar", () => {
		current.engine = BUILT;
		render(<App />);

		expect(screen.getByTestId("engine-version").textContent).toBe("19.3.0");
	});

	it("says the engine is not built, and how to fix it", () => {
		current.engine = UNBUILT;
		render(<App />);

		const notice = screen.getByTestId("engine-unbuilt");
		expect(notice.textContent).toContain("not built");
		/* The command, verbatim, because a notice that says something is
		 * missing without saying how to get it is half a notice. */
		expect(notice.textContent).toContain(UNBUILT.buildCommand);
		expect(screen.getByTestId("status-phase").textContent).toBe("not built");
	});

	it("still offers the canvas when the engine is not built", () => {
		/*
		 * The engine looks its canvas up by selector at boot. If the element
		 * only appeared once the module had loaded, there would be nothing for
		 * it to find — so it exists in both states, and the notice sits over it.
		 */
		current.engine = UNBUILT;
		render(<App />);

		const canvas = screen.getByTestId("engine-canvas");
		expect(canvas.tagName).toBe("CANVAS");
		/* The id, not the test id. Ten call sites in the C tree hardcode
		 * "#canvas" and renaming it costs the GL context and every pointer
		 * callback, silently. */
		expect(canvas.id).toBe("canvas");
	});

	it("mounts the canvas exactly once", () => {
		current.engine = BUILT;
		render(<App />);

		expect(screen.getAllByTestId("engine-canvas")).toHaveLength(1);
	});
});
