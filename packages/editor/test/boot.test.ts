// SPDX-License-Identifier: GPL-2.0-or-later
//
// What bootEngine puts on the page, and what it refuses to do twice.
//
// The module itself is never loaded here — jsdom will not execute an injected
// classic script against a WASM binary, and a test that faked one would be
// asserting against its own fake. What is worth asserting is everything up to
// that point: the global the loader reads, the callbacks the engine's EM_JS
// bridges look for, and the guard that keeps React's strict-mode double-mount
// from injecting two copies of a module that owns a canvas and a main loop.
//
// The module actually running is the browser suite's job. See e2e/boot.spec.ts.

import { beforeEach, describe, expect, it, vi } from "vitest";

import { bootEngine, resetBootStateForTests } from "../src/engine/boot.js";
import type { EngineInfo, EngineStatus } from "../src/engine/types.js";

const BUILT: EngineInfo = {
	built: true,
	version: "19.2.1",
	exports: ["_main"],
	base: "/engine/",
	buildCommand: "pnpm --filter @kruddage/engine run build",
};

const UNBUILT: EngineInfo = { ...BUILT, built: false, version: null };

interface HostHooks {
	Module?: { locateFile: (path: string, prefix: string) => string };
	kruddSetRunning?: () => void;
	kruddSetReady?: () => void;
	kruddSetRenderer?: (name: string) => void;
	kruddSetRendererFailed?: (name: string, why: string) => void;
	kruddBootGame?: () => string;
}

function host(): HostHooks {
	return window as unknown as HostHooks;
}

function loaderScripts(): HTMLScriptElement[] {
	return [...document.querySelectorAll<HTMLScriptElement>("script[src*='index.js']")];
}

beforeEach(() => {
	resetBootStateForTests();
	document.body.innerHTML = "";
	for (const key of [
		"kruddSetRunning",
		"kruddSetReady",
		"kruddSetRenderer",
		"kruddSetRendererFailed",
		"kruddBootGame",
	] as const) {
		delete host()[key];
	}
});

describe("bootEngine", () => {
	it("reports unbuilt and touches nothing when there is no engine", () => {
		const onStatus = vi.fn<(s: EngineStatus) => void>();

		bootEngine({ engine: UNBUILT, canvas: canvas(), onStatus });

		expect(onStatus).toHaveBeenCalledWith({
			phase: "unbuilt",
			renderer: null,
			error: null,
		});
		expect(loaderScripts()).toHaveLength(0);
		expect(host().kruddSetRunning).toBeUndefined();
	});

	it("injects the loader from the engine's own base", () => {
		bootEngine({ engine: BUILT, canvas: canvas(), onStatus: vi.fn() });

		const scripts = loaderScripts();
		expect(scripts).toHaveLength(1);
		expect(scripts[0]?.getAttribute("src")).toBe("/engine/index.js");
	});

	it("maps locateFile to the identity", () => {
		bootEngine({ engine: BUILT, canvas: canvas(), onStatus: vi.fn() });

		/* The deployed shell applies a cache-busting stem here; the editor ships
		 * its artifacts unrenamed, so applying one would ask for a URL that was
		 * never staged. This test is the guard on that difference — see the note
		 * at the top of boot.ts. */
		expect(host().Module?.locateFile("index.wasm", "/engine/")).toBe(
			"/engine/index.wasm"
		);
	});

	it("routes the engine's own callbacks into status", () => {
		const seen: EngineStatus[] = [];
		bootEngine({
			engine: BUILT,
			canvas: canvas(),
			onStatus: (s) => seen.push(s),
		});

		host().kruddSetRunning?.();
		host().kruddSetRenderer?.("webgpu");
		host().kruddSetReady?.();

		expect(seen.at(-1)).toEqual({
			phase: "ready",
			renderer: "webgpu",
			error: null,
		});
	});

	it("carries the reason a backend failed", () => {
		const seen: EngineStatus[] = [];
		bootEngine({
			engine: BUILT,
			canvas: canvas(),
			onStatus: (s) => seen.push(s),
		});

		host().kruddSetRendererFailed?.("webgpu", "no adapter");

		expect(seen.at(-1)).toEqual({
			phase: "failed",
			renderer: "webgpu",
			error: "no adapter",
		});
	});

	it("leaves the launcher standing rather than auto-loading a game", () => {
		bootEngine({ engine: BUILT, canvas: canvas(), onStatus: vi.fn() });

		expect(host().kruddBootGame?.()).toBe("none");
	});

	it("refuses to boot a second module on a remount", () => {
		bootEngine({ engine: BUILT, canvas: canvas(), onStatus: vi.fn() });
		bootEngine({ engine: BUILT, canvas: canvas(), onStatus: vi.fn() });

		expect(loaderScripts()).toHaveLength(1);
	});

	it("reports the last known status to a late caller", () => {
		bootEngine({ engine: BUILT, canvas: canvas(), onStatus: vi.fn() });
		host().kruddSetRenderer?.("webgl");

		const onStatus = vi.fn<(s: EngineStatus) => void>();
		bootEngine({ engine: BUILT, canvas: canvas(), onStatus });

		/* A second mount must not be told "loading" when the engine is already
		 * up — that is how a status strip ends up permanently stuck. */
		expect(onStatus).toHaveBeenCalledWith(
			expect.objectContaining({ renderer: "webgl" })
		);
	});

	it("removes its hooks on dispose but leaves the engine alone", () => {
		const handle = bootEngine({
			engine: BUILT,
			canvas: canvas(),
			onStatus: vi.fn(),
		});

		handle.dispose();

		expect(host().kruddSetRunning).toBeUndefined();
		expect(loaderScripts()).toHaveLength(0);
	});
});

function canvas(): HTMLCanvasElement {
	const element = document.createElement("canvas");
	document.body.appendChild(element);
	return element;
}
