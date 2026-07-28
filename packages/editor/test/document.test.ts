// SPDX-License-Identifier: GPL-2.0-or-later
//
// The document model (#947), against the real bridge client.
//
// What is stubbed is the wasm module, not the Bridge — see bridge-stub.ts. So
// every assertion below about "the editor sent X" is an assertion about bytes
// that were actually encoded, and a document that drove the boundary wrongly
// fails here rather than in a browser.

import { OP } from "@kruddage/engine/bridge";
import { beforeEach, describe, expect, it, vi } from "vitest";

import { COMMANDS, isCommandId } from "../src/document/commands.js";
import { createDocument, type KruddDocument } from "../src/document/document.js";
import {
	manualScheduler,
	moduleStub,
	readString,
	type ModuleStub,
} from "./bridge-stub.js";

import { createBridge } from "@kruddage/engine/bridge";

let module: ModuleStub;
let loop: ReturnType<typeof manualScheduler>;
let document: KruddDocument;

function build(): void {
	module = moduleStub();
	loop = manualScheduler();
	document = createDocument({
		bridge: createBridge(module),
		scheduler: loop.scheduler,
	});
	document.run();
}

beforeEach(build);

describe("the frame loop", () => {
	it("flushes once per frame and not otherwise", () => {
		document.dispatch("history.undo", {});
		expect(module.tapes).toHaveLength(0);

		loop.frame();
		expect(module.tapes).toHaveLength(1);
	});

	it("does not cross the boundary when there is nothing to say", () => {
		loop.frame();
		loop.frame();
		/*
		 * A page with no panels mounted watches nothing and sends nothing. It
		 * must not pay a wasm call a frame to be told the same thing.
		 */
		expect(module.tapes).toHaveLength(0);
	});

	it("stops when the returned function is called", () => {
		const stop = document.run();
		expect(loop.running).toBe(true);
		stop();
		expect(loop.running).toBe(false);
	});
});

describe("commands", () => {
	it("every command id maps to a spec, and the guard agrees", () => {
		for (const id of Object.keys(COMMANDS)) {
			expect(isCommandId(id)).toBe(true);
			expect(COMMANDS[id as keyof typeof COMMANDS].label).toBeTruthy();
		}
		expect(isCommandId("entity.explode")).toBe(false);
	});

	it("a mutation becomes the op the engine expects", () => {
		document.dispatch("entity.rename", { id: 4, name: "crate" });
		loop.frame();

		const [record] = module.tapes[0]!;
		expect(record!.op).toBe(OP.ENTITY_NAME);
		/* Four bytes of id, then the length-prefixed name. */
		expect(readString(record!.payload, 4)).toBe("crate");
	});

	it("selection is a command, not local state", () => {
		document.dispatch("selection.set", { id: 7 });
		loop.frame();

		expect(module.ops).toEqual([OP.SELECT]);
	});

	it("undo and redo are ordinary commands", () => {
		document.dispatch("history.undo", {});
		document.dispatch("history.redo", {});
		loop.frame();

		expect(module.ops).toEqual([OP.UNDO, OP.REDO]);
	});

	it("keeps no history of its own", () => {
		/*
		 * #944's Q2: `world/edit`'s ring is the only undo stack in the system.
		 * The absence of a local one is a design decision, so it is asserted
		 * rather than left as something a later PR can add without noticing.
		 */
		expect(Object.keys(document)).not.toContain("history");
		expect(Object.keys(document)).not.toContain("undoStack");
		expect(document).not.toHaveProperty("undo");
	});

	it("creates an entity at identity rather than at zero scale", () => {
		document.dispatch("entity.create", { parent: -1 });
		loop.frame();

		const [record] = module.tapes[0]!;
		const view = new DataView(
			record!.payload.buffer,
			record!.payload.byteOffset,
			record!.payload.byteLength
		);
		/* parent, then ten floats: position, rotation, scale. */
		expect(view.getInt32(0, true)).toBe(-1);
		expect(view.getFloat32(4 + 6 * 4, true)).toBe(1); /* rotation w */
		expect(view.getFloat32(4 + 7 * 4, true)).toBe(1); /* scale x */
	});
});

describe("gestures", () => {
	it("brackets a drag into one undo entry", () => {
		const drag = document.gesture("Move");
		document.dispatch("entity.transform", {
			id: 1,
			transform: { position: [1, 0, 0] },
		});
		document.dispatch("entity.transform", {
			id: 1,
			transform: { position: [2, 0, 0] },
		});
		drag.commit();
		loop.frame();

		expect(module.ops).toEqual([
			OP.GESTURE_BEGIN,
			OP.ENTITY_TRANSFORM,
			OP.ENTITY_TRANSFORM,
			OP.GESTURE_COMMIT,
		]);
	});

	it("carries the label the history will show", () => {
		document.gesture("Move").commit();
		loop.frame();

		expect(readString(module.tapes[0]![0]!.payload)).toBe("Move");
	});

	it("settles exactly once", () => {
		/*
		 * A pointerup handler firing twice is a browser behaviour, not a caller
		 * mistake — and an unbalanced commit would close a gesture the next
		 * drag had opened, folding two drags into one undo entry.
		 */
		const drag = document.gesture("Move");
		drag.commit();
		drag.commit();
		drag.abort();
		loop.frame();

		expect(module.ops).toEqual([OP.GESTURE_BEGIN, OP.GESTURE_COMMIT]);
	});

	it("reports its depth while it is open", () => {
		expect(document.state().gestureDepth).toBe(0);
		const drag = document.gesture("Move");
		expect(document.state().gestureDepth).toBe(1);
		drag.abort();
		expect(document.state().gestureDepth).toBe(0);
	});
});

describe("dirty state", () => {
	it("opens clean, whatever generation the engine is already at", () => {
		module.willAnswer({ generations: { scene: 9, selection: 1, history: 1 } });
		document.dispatch("history.undo", {});
		loop.frame();

		/*
		 * The baseline is the world as the editor found it. Seeding from 0
		 * would mark an untouched document dirty on its first frame, which
		 * teaches a reader to ignore the marker.
		 */
		expect(document.state().dirty).toBe(false);
	});

	it("goes dirty when the scene generation moves", () => {
		module.willAnswer({ generations: { scene: 1, selection: 1, history: 1 } });
		document.dispatch("history.undo", {});
		loop.frame();

		module.willAnswer({ generations: { scene: 2, selection: 1, history: 1 } });
		document.dispatch("entity.rename", { id: 1, name: "x" });
		loop.frame();

		expect(document.state().dirty).toBe(true);
	});

	it("ignores a change that is not the scene's", () => {
		module.willAnswer({ generations: { scene: 1, selection: 1, history: 1 } });
		document.dispatch("history.undo", {});
		loop.frame();

		module.willAnswer({ generations: { scene: 1, selection: 5, history: 3 } });
		document.dispatch("selection.set", { id: 2 });
		loop.frame();

		/* Selecting something is not editing it. */
		expect(document.state().dirty).toBe(false);
	});

	it("notifies only when the state actually changed", () => {
		const listener = vi.fn();
		document.subscribeState(listener);

		module.willAnswer({ generations: { scene: 1, selection: 1, history: 1 } });
		document.dispatch("history.undo", {});
		loop.frame();

		module.willAnswer({ generations: { scene: 2, selection: 1, history: 1 } });
		document.dispatch("entity.rename", { id: 1, name: "x" });
		loop.frame();

		module.willAnswer({ generations: { scene: 3, selection: 1, history: 1 } });
		document.dispatch("entity.rename", { id: 1, name: "y" });
		loop.frame();

		/*
		 * Clean -> dirty is one change. Dirty -> dirtier is not a change, and
		 * publishing it would re-render every panel reading this, once a frame,
		 * to show the same two booleans.
		 */
		expect(listener).toHaveBeenCalledTimes(1);
	});
});

describe("save", () => {
	it("asks once rather than watching, and returns the form", async () => {
		module.willAnswer({
			results: [
				{
					kind: "scene.text",
					generation: 3,
					fresh: false,
					value: "(scene saved)",
				},
			],
		});
		const saving = document.save();
		loop.frame();

		expect(await saving).toBe("(scene saved)");
		expect(module.ops).toEqual([OP.QUERY_SCENE_TEXT]);
	});

	it("does not leave the scene text in the cache", async () => {
		module.willAnswer({
			results: [
				{ kind: "scene.text", generation: 3, fresh: false, value: "(scene s)" },
			],
		});
		const saving = document.save();
		loop.frame();
		await saving;

		/* An entire scene, from whenever the last save happened. */
		expect(document.bridge.read("scene.text")).toBeNull();
	});

	it("marks the document clean at the generation it wrote", async () => {
		module.willAnswer({ generations: { scene: 1, selection: 1, history: 1 } });
		document.dispatch("history.undo", {});
		loop.frame();

		module.willAnswer({ generations: { scene: 2, selection: 1, history: 1 } });
		document.dispatch("entity.rename", { id: 1, name: "x" });
		loop.frame();
		expect(document.state().dirty).toBe(true);

		module.willAnswer({
			generations: { scene: 2, selection: 1, history: 1 },
			results: [
				{ kind: "scene.text", generation: 2, fresh: false, value: "(scene s)" },
			],
		});
		const saving = document.save();
		loop.frame();
		await saving;

		expect(document.state().dirty).toBe(false);
	});

	it("rejects rather than resolving with null", async () => {
		/*
		 * null is how the engine says it could not write a scene. A caller that
		 * wrote the resolved value to storage would otherwise save the word
		 * "null" over a project.
		 */
		module.willAnswer({
			results: [
				{ kind: "scene.text", generation: 1, fresh: false, value: null },
			],
		});
		const saving = document.save();
		loop.frame();

		await expect(saving).rejects.toThrow(/could not write this scene/);
	});
});

describe("load", () => {
	it("sends the form and resolves once the engine has it", async () => {
		const loading = document.load("(scene incoming)");
		loop.frame();
		await loading;

		expect(module.ops).toEqual([OP.SCENE_LOAD, OP.QUERY_SELECTION]);
		expect(readString(module.tapes[0]![0]!.payload)).toBe("(scene incoming)");
	});

	it("resolves even when the loaded scene is the one already open", async () => {
		/*
		 * An identical scene moves no generation and notifies nobody. A load
		 * that waited for a change would hang here, and "open the file I
		 * already have open" is not an exotic thing for a reader to do.
		 */
		module.willAnswer({ generations: { scene: 1, selection: 1, history: 1 } });
		await expect(
			(async () => {
				const loading = document.load("(scene same)");
				loop.frame();
				return loading;
			})()
		).resolves.toBeUndefined();
	});

	it("leaves the document clean at what was just loaded", async () => {
		module.willAnswer({ generations: { scene: 7, selection: 1, history: 1 } });
		const loading = document.load("(scene fresh)");
		loop.frame();
		await loading;

		expect(document.state().dirty).toBe(false);
	});
});
