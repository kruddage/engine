// SPDX-License-Identifier: GPL-2.0-or-later
//
// The application's two states: an engine that is there, and one that is not.
//
// The unbuilt branch is the one worth a test. It is the case a contributor
// without emsdk hits on their first `pnpm dev`, it is the case CI's workspace
// job runs in, and it is the one where rendering an empty box instead of an
// explanation would be indistinguishable from the editor being broken.

import { describe, expect, it, vi } from "vitest";
import { render, screen } from "@testing-library/react";

import type { EngineInfo } from "../src/engine/types.js";

const BUILT: EngineInfo = {
	built: true,
	version: "19.2.1",
	exports: ["_main", "_krudd_load_game"],
	base: "/engine/",
	buildCommand: "pnpm --filter @kruddage/engine run build",
};

const UNBUILT: EngineInfo = {
	built: false,
	version: null,
	exports: [],
	base: "/engine/",
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

describe("App", () => {
	it("shows the engine's identity when it is built", () => {
		current.engine = BUILT;
		render(<App />);

		expect(screen.getByTestId("engine-version").textContent).toBe("19.2.1");
		expect(screen.getByTestId("engine-exports").textContent).toContain(
			"_krudd_load_game"
		);
		expect(screen.queryByTestId("engine-unbuilt")).toBeNull();
	});

	it("says the engine is not built, and how to fix it", () => {
		current.engine = UNBUILT;
		render(<App />);

		const notice = screen.getByTestId("engine-unbuilt");
		expect(notice).toBeTruthy();
		/* The command is the whole value of the notice. A message that said
		 * "not built" without saying what to run would be an empty box with
		 * extra words. */
		expect(notice.textContent).toContain(
			"pnpm --filter @kruddage/engine run build"
		);
		expect(screen.queryByTestId("engine-identity")).toBeNull();
	});

	it("reports the phase in the status strip", () => {
		current.engine = UNBUILT;
		render(<App />);

		expect(screen.getByTestId("status-phase").textContent).toBe("not built");
	});

	it("always offers a canvas for the engine to take", () => {
		current.engine = BUILT;
		render(<App />);

		/* The viewport is where #949 hands this element to the engine. It exists
		 * from the first render rather than appearing once the module loads —
		 * boot.ts needs it before the loader is injected, not after. */
		expect(screen.getByTestId("engine-canvas").tagName).toBe("CANVAS");
	});

	/* The engine looks the canvas up by selector rather than taking a handle:
	 * renderer_webgl.c creates the GL context against "#canvas", and
	 * kruddgui.cpp binds every pointer callback and both sizing calls to it.
	 * Rename the element and there is no context and no input, reported nowhere
	 * near the cause — an unchecked create_context return, a subsystem that
	 * logs "init" anyway, and a TypeError from the first glCreateShader.
	 *
	 * jsdom cannot catch that, so this asserts the contract directly. It is
	 * cheap and it is the guard on three CI runs' worth of diagnosis. */
	it("gives the canvas the id the engine looks it up by", () => {
		current.engine = BUILT;
		render(<App />);

		expect(screen.getByTestId("engine-canvas").id).toBe("canvas");
		expect(document.querySelector("#canvas")).not.toBeNull();
	});
});
