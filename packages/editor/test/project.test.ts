// SPDX-License-Identifier: GPL-2.0-or-later
//
// The seam between #948's shell and #947's document: File > Save reaching an
// engine, and every way that can go wrong reaching the reader.
//
// runAction is tested here rather than in commands.test.ts because what is
// under test is the routing — that an id the shell does not handle still falls
// through to the hint once a document exists, and that the five it does handle
// stop hinting. commands.test.ts owns the menu table itself.

import { createBridge, OP } from "@kruddage/engine/bridge";
import { beforeEach, describe, expect, it, vi } from "vitest";

import { createDocument, type KruddDocument } from "../src/document/document.js";
import {
	clearProject,
	EMPTY_SCENE,
	loadProject,
	memoryStore,
	saveProject,
	type ProjectStore,
} from "../src/document/project-store.js";
import { runAction, type DocumentActions } from "../src/shell/commands.js";
import { manualScheduler, moduleStub, type ModuleStub } from "./bridge-stub.js";

let module: ModuleStub;
let loop: ReturnType<typeof manualScheduler>;
let document: KruddDocument;
let store: ProjectStore;

beforeEach(() => {
	module = moduleStub();
	loop = manualScheduler();
	document = createDocument({
		bridge: createBridge(module),
		scheduler: loop.scheduler,
	});
	document.run();
	store = memoryStore();
});

/*
 * useProject as a plain function. The hook is a useMemo around exactly this, so
 * exercising it without React keeps these assertions about behaviour rather
 * than about render counts — and the hook itself is covered where it matters,
 * through the shell.
 */
function verbs(hint: (text: string) => void): DocumentActions {
	return {
		undo: () => document.dispatch("history.undo", {}),
		redo: () => document.dispatch("history.redo", {}),
		newProject: () => void document.load(EMPTY_SCENE).then(() => hint("new project")),
		open: () => {
			const text = loadProject(store);
			if (text === null) {
				hint("no saved project to open");
				return;
			}
			void document.load(text).then(() => hint("project opened"));
		},
		save: () => {
			void document
				.save()
				.then((text) =>
					hint(saveProject(store, text) ? "project saved" : "save failed")
				)
				.catch((error: unknown) => hint(`save failed: ${String(error)}`));
		},
	};
}

describe("the project store", () => {
	it("round-trips the text it was given", () => {
		expect(loadProject(store)).toBeNull();
		expect(saveProject(store, "(scene a)")).toBe(true);
		expect(loadProject(store)).toBe("(scene a)");
		clearProject(store);
		expect(loadProject(store)).toBeNull();
	});

	it("reports a refused write rather than claiming success", () => {
		/*
		 * Quota, private mode, a blocked origin. This is the one failure that
		 * costs a reader real work and stays invisible until they reload, which
		 * is why saveProject returns a boolean where saveLayout swallows.
		 */
		const refusing: ProjectStore = {
			getItem: () => null,
			setItem: () => {
				throw new Error("QuotaExceededError");
			},
			removeItem: () => {},
		};
		expect(saveProject(refusing, "(scene a)")).toBe(false);
	});

	it("offers an empty document as a scene form, not an empty string", () => {
		/*
		 * New goes through the same load path as Open, so that clearing the
		 * world, resetting the history and marking the document clean all
		 * happen once, in the engine, in the order the load op defines.
		 */
		expect(EMPTY_SCENE).toMatch(/^\(scene\s/);
	});
});

describe("menu actions reach the engine", () => {
	const context = (document?: DocumentActions) => ({
		resetLayout: vi.fn(),
		togglePanel: vi.fn(),
		movePanel: vi.fn(),
		hint: vi.fn(),
		...(document ? { document } : {}),
	});

	it("undo and redo become commands", () => {
		const ctx = context(verbs(vi.fn()));
		runAction("undo", "&Undo", ctx);
		runAction("redo", "&Redo", ctx);
		loop.frame();

		expect(module.ops).toEqual([OP.UNDO, OP.REDO]);
		expect(ctx.hint).not.toHaveBeenCalled();
	});

	it("hints instead, while there is no document yet", () => {
		/*
		 * Every frame before the wasm runtime is up, and forever on a page whose
		 * engine was never built. "coming soon" is the same thing the shell says
		 * for an action nobody has written, and it is the honest answer here
		 * too — the button genuinely cannot work.
		 */
		const ctx = context();
		runAction("undo", "&Undo", ctx);

		expect(module.ops).toEqual([]);
		expect(ctx.hint).toHaveBeenCalledWith("Undo: coming soon");
	});

	it("leaves the layout actions alone", () => {
		const ctx = context(verbs(vi.fn()));
		runAction("reset-layout", "Reset &Layout", ctx);

		expect(ctx.resetLayout).toHaveBeenCalled();
		expect(module.ops).toEqual([]);
	});

	it("still hints for an action nobody has wired", () => {
		const ctx = context(verbs(vi.fn()));
		runAction("paste", "&Paste", ctx);

		expect(ctx.hint).toHaveBeenCalledWith("Paste: coming soon");
	});
});

describe("save and open", () => {
	it("stores what the engine wrote, and says so", async () => {
		const hint = vi.fn();
		module.willAnswer({
			results: [
				{
					kind: "scene.text",
					generation: 2,
					fresh: false,
					value: "(scene mine)",
				},
			],
		});
		verbs(hint).save();
		loop.frame();
		await vi.waitFor(() => expect(hint).toHaveBeenCalled());

		expect(loadProject(store)).toBe("(scene mine)");
		expect(hint).toHaveBeenCalledWith("project saved");
	});

	it("says so when the engine could not write one", async () => {
		const hint = vi.fn();
		module.willAnswer({
			results: [
				{ kind: "scene.text", generation: 2, fresh: false, value: null },
			],
		});
		verbs(hint).save();
		loop.frame();
		await vi.waitFor(() => expect(hint).toHaveBeenCalled());

		expect(hint.mock.calls[0]![0]).toMatch(/save failed/);
		/* And nothing was written over the project that was already there. */
		expect(loadProject(store)).toBeNull();
	});

	it("refuses to open what was never saved", () => {
		const hint = vi.fn();
		verbs(hint).open();

		expect(hint).toHaveBeenCalledWith("no saved project to open");
		expect(module.ops).toEqual([]);
	});

	it("sends a stored project back to the engine", async () => {
		const hint = vi.fn();
		saveProject(store, "(scene stored)");
		verbs(hint).open();
		loop.frame();
		await vi.waitFor(() => expect(hint).toHaveBeenCalled());

		expect(module.ops).toContain(OP.SCENE_LOAD);
		expect(hint).toHaveBeenCalledWith("project opened");
	});

	it("round-trips a document through save and open unchanged", async () => {
		const hint = vi.fn();
		const form = "(scene round\n  (entity (name \"crate\") (at 1 2 3)))\n";

		module.willAnswer({
			results: [
				{ kind: "scene.text", generation: 2, fresh: false, value: form },
			],
		});
		verbs(hint).save();
		loop.frame();
		await vi.waitFor(() => expect(hint).toHaveBeenCalledWith("project saved"));

		verbs(hint).open();
		loop.frame();

		/*
		 * The bytes the engine wrote are the bytes it is handed back. Whether
		 * the engine rebuilds the same world from them is asserted in C, where
		 * both halves of the form live (scene_script_test).
		 */
		const sent = module.tapes.at(-1)!.find((r) => r.op === OP.SCENE_LOAD)!;
		const view = new DataView(
			sent.payload.buffer,
			sent.payload.byteOffset,
			sent.payload.byteLength
		);
		const length = view.getUint16(0, true);
		expect(new TextDecoder().decode(sent.payload.subarray(2, 2 + length))).toBe(
			form
		);
	});
});
