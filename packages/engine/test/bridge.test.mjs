// SPDX-License-Identifier: GPL-2.0-or-later
//
// The boundary's client half, without a wasm module.
//
// The fake engine below decodes the tape the client wrote — it does not just
// record it — so an encoder that drifts from the format the C decoder reads
// fails here rather than in a browser. The reply side is canned, because what
// is under test on that side is the cache, not the engine.
//
// The C half's own suite (krudd/engine/ui/bridge/bridge_test.c) asserts the
// other direction: a real tape in, the real reply document out. Between them
// the format is pinned from both ends.

import assert from "node:assert/strict";
import test from "node:test";

import {
	BRIDGE_PROTOCOL,
	DRAG_PHASE,
	GIZMO_MODE,
	OP,
	Tape,
	TAPE_MAGIC,
	createBridge,
	protocolMatches,
} from "../src/bridge.mjs";

/* ------------------------------------------------------------------ *
 * A tape reader — the C decoder's rules, in JS
 * ------------------------------------------------------------------ */

function decodeTape(bytes) {
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	assert.equal(view.getUint32(0, true), TAPE_MAGIC, "tape magic");
	const count = view.getUint32(4, true);
	const records = [];
	let pos = 8;
	for (let i = 0; i < count; i++) {
		const op = view.getUint16(pos, true);
		const len = view.getUint16(pos + 2, true);
		pos += 4;
		records.push({ op, payload: bytes.subarray(pos, pos + len) });
		pos += len;
	}
	assert.equal(pos, bytes.length, "records account for every byte");
	return records;
}

function payloadReader(payload) {
	const view = new DataView(
		payload.buffer,
		payload.byteOffset,
		payload.byteLength
	);
	let pos = 0;
	return {
		i32: () => {
			const v = view.getInt32(pos, true);
			pos += 4;
			return v;
		},
		u32: () => {
			const v = view.getUint32(pos, true);
			pos += 4;
			return v;
		},
		f32: () => {
			const v = view.getFloat32(pos, true);
			pos += 4;
			return v;
		},
		str: () => {
			const n = view.getUint16(pos, true);
			pos += 2;
			const s = new TextDecoder().decode(payload.subarray(pos, pos + n));
			pos += n;
			return s;
		},
		get done() {
			return pos === payload.length;
		},
	};
}

/* ------------------------------------------------------------------ *
 * A fake module
 * ------------------------------------------------------------------ */

const BUFFER_AT = 1024;

function fakeModule({ capacity = 64 * 1024, protocol = BRIDGE_PROTOCOL } = {}) {
	const heap = new Uint8Array(BUFFER_AT + capacity + 4096);
	const replies = [];
	const seen = [];
	let replyText = "";

	return {
		HEAPU8: heap,
		UTF8ToString: () => replyText,
		_krudd_bridge_protocol: () => protocol,
		_krudd_bridge_buffer: () => BUFFER_AT,
		_krudd_bridge_capacity: () => capacity,
		_krudd_bridge_exchange(len) {
			/* this.HEAPU8, not the captured `heap`: the test that swaps the
			 * view out is asserting the client re-reads it, and a fake that
			 * held its own reference would pass regardless. */
			seen.push(
				decodeTape(this.HEAPU8.subarray(BUFFER_AT, BUFFER_AT + len))
			);
			replyText = JSON.stringify(
				replies.shift() ?? reply({ serial: seen.length })
			);
			return 1;
		},
		/** Queue what the next exchange answers with. */
		willAnswer(next) {
			replies.push(next);
		},
		/** The records decoded from each exchange so far. */
		get tapes() {
			return seen;
		},
	};
}

function reply(overrides = {}) {
	return {
		protocol: BRIDGE_PROTOCOL,
		serial: 1,
		applied: 0,
		error: null,
		code: 0,
		generations: { scene: 1, selection: 1, history: 1, viewport: 1 },
		events: [],
		eventsDropped: 0,
		results: [],
		...overrides,
	};
}

/* ------------------------------------------------------------------ *
 * The encoder
 * ------------------------------------------------------------------ */

test("an empty tape is a header and nothing else", () => {
	const tape = new Tape();
	assert.equal(tape.length, 8);
	assert.equal(tape.count, 0);
	assert.deepEqual(decodeTape(tape.bytes()), []);
});

test("the tape grows past its initial capacity", () => {
	const tape = new Tape(16);
	for (let i = 0; i < 200; i++) tape.open(OP.SELECT).i32(i).close();
	const records = decodeTape(tape.bytes());
	assert.equal(records.length, 200);
	assert.equal(payloadReader(records[199].payload).i32(), 199);
});

test("a transform is ten floats, and missing parts are identity", () => {
	const tape = new Tape();
	tape.open(OP.ENTITY_TRANSFORM).i32(3).transform({ position: [1, 2, 3] }).close();
	const [record] = decodeTape(tape.bytes());

	assert.equal(record.payload.length, 4 + 40);
	const r = payloadReader(record.payload);
	assert.equal(r.i32(), 3);
	assert.deepEqual([r.f32(), r.f32(), r.f32()], [1, 2, 3]);
	assert.deepEqual([r.f32(), r.f32(), r.f32(), r.f32()], [0, 0, 0, 1]);
	/*
	 * Identity, not zero. A default of zero here would collapse the entity
	 * to nothing, and "the caller omitted scale" must not be how that
	 * happens.
	 */
	assert.deepEqual([r.f32(), r.f32(), r.f32()], [1, 1, 1]);
	assert.ok(r.done);
});

test("a string is length-prefixed UTF-8, counted in bytes", () => {
	const tape = new Tape();
	tape.open(OP.ENTITY_NAME).i32(1).str("héllo").close();
	const [record] = decodeTape(tape.bytes());
	const r = payloadReader(record.payload);

	assert.equal(r.i32(), 1);
	assert.equal(r.str(), "héllo");
	assert.ok(r.done);
	/* Six bytes for five characters — the prefix counts bytes, not code
	 * points, which is what the C side reads. */
	assert.equal(record.payload.length, 4 + 2 + 6);
});

/* ------------------------------------------------------------------ *
 * Commands
 * ------------------------------------------------------------------ */

test("nothing crosses when nothing is queued and nothing is watched", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	assert.equal(bridge.idle, true);
	assert.equal(bridge.flush(), null);
	assert.equal(module.tapes.length, 0);
});

test("commands are sent in the order they were queued", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.beginGesture("Move");
	bridge.setTransform(2, { position: [0, 5, 0] });
	bridge.commitGesture();
	bridge.flush();

	const [tape] = module.tapes;
	assert.deepEqual(
		tape.map((r) => r.op),
		[OP.GESTURE_BEGIN, OP.ENTITY_TRANSFORM, OP.GESTURE_COMMIT]
	);
	assert.equal(payloadReader(tape[0].payload).str(), "Move");
});

test("a flushed batch is not sent twice", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.select(4);
	bridge.flush();
	bridge.watch("selection");
	bridge.flush();

	assert.deepEqual(
		module.tapes[0].map((r) => r.op),
		[OP.SELECT]
	);
	/* Only the query — resending a command the engine may have applied
	 * would apply it twice. */
	assert.deepEqual(
		module.tapes[1].map((r) => r.op),
		[OP.QUERY_SELECTION]
	);
});

test("a failed exchange still clears the batch", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	module.willAnswer(reply({ error: "payload", code: -4 }));
	bridge.destroyEntity(9);
	bridge.flush();
	bridge.watch("history");
	bridge.flush();

	assert.deepEqual(
		module.tapes[1].map((r) => r.op),
		[OP.QUERY_HISTORY]
	);
});

test("undo and redo are ordinary commands", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.undo();
	bridge.redo();
	bridge.flush();

	assert.deepEqual(
		module.tapes[0].map((r) => r.op),
		[OP.UNDO, OP.REDO]
	);
	/* And nothing local moved: there is no client-side history to move. */
	assert.equal(bridge.read("history"), null);
});

/* ------------------------------------------------------------------ *
 * The read model
 * ------------------------------------------------------------------ */

test("a watched query is asked with generation 0 until it has an answer", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.watch("scene.tree");
	module.willAnswer(
		reply({
			results: [
				{
					kind: "scene.tree",
					generation: 4,
					fresh: false,
					value: { paused: false, entities: [] },
				},
			],
		})
	);
	bridge.flush();

	assert.equal(payloadReader(module.tapes[0][0].payload).u32(), 0);
	assert.deepEqual(bridge.read("scene.tree"), { paused: false, entities: [] });

	/* The second ask carries what we now hold. */
	bridge.flush();
	assert.equal(payloadReader(module.tapes[1][0].payload).u32(), 4);
});

test("a fresh result leaves the cached value in place", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.watch("scene.tree");
	module.willAnswer(
		reply({
			results: [
				{
					kind: "scene.tree",
					generation: 7,
					fresh: false,
					value: { paused: false, entities: [{ id: 0 }] },
				},
			],
		})
	);
	bridge.flush();
	const first = bridge.read("scene.tree");

	module.willAnswer(
		reply({ results: [{ kind: "scene.tree", generation: 7, fresh: true }] })
	);
	bridge.flush();

	/*
	 * Identity, not just equality. A hit must hand back the very same object
	 * — a structurally equal copy would make every memoized consumer
	 * downstream re-render on a tree that did not change, which is the cost
	 * the generation exists to avoid.
	 */
	assert.equal(bridge.read("scene.tree"), first);
	assert.deepEqual(first, { paused: false, entities: [{ id: 0 }] });
});

test("entity queries are cached per id", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.watch("entity", 1);
	bridge.watch("entity", 2);
	module.willAnswer(
		reply({
			results: [
				{
					kind: "entity",
					id: 1,
					generation: 2,
					fresh: false,
					value: { id: 1, name: "one" },
				},
				{
					kind: "entity",
					id: 2,
					generation: 2,
					fresh: false,
					value: { id: 2, name: "two" },
				},
			],
		})
	);
	bridge.flush();

	assert.equal(bridge.read("entity", 1).name, "one");
	assert.equal(bridge.read("entity", 2).name, "two");
	assert.equal(bridge.read("entity", 3), null);
});

test("watching is reference counted, and the last release drops the cache", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	const a = bridge.watch("selection");
	const b = bridge.watch("selection");
	module.willAnswer(
		reply({
			results: [
				{
					kind: "selection",
					generation: 3,
					fresh: false,
					value: { id: 5 },
				},
			],
		})
	);
	bridge.flush();
	assert.deepEqual(bridge.read("selection"), { id: 5 });

	a();
	/* One watcher left: still asked, still cached. */
	assert.deepEqual(
		module.tapes.at(-1).map((r) => r.op),
		[OP.QUERY_SELECTION]
	);
	assert.deepEqual(bridge.read("selection"), { id: 5 });

	b();
	/*
	 * The value goes with the last watcher. Keeping it would let a later
	 * watcher read a value from before the gap, stamped with a generation
	 * claiming it is current.
	 */
	assert.equal(bridge.read("selection"), null);
	assert.equal(bridge.idle, true);
});

test("a null value is an answer, not a miss", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.watch("entity", 7);
	module.willAnswer(
		reply({
			results: [
				{ kind: "entity", id: 7, generation: 9, fresh: false, value: null },
			],
		})
	);
	bridge.flush();

	/* The engine said "there is no entity 7". The next ask still carries 9,
	 * so the engine can answer `fresh` rather than re-deciding. */
	assert.equal(bridge.read("entity", 7), null);
	bridge.flush();
	const r = payloadReader(module.tapes[1][0].payload);
	assert.equal(r.i32(), 7);
	assert.equal(r.u32(), 9);
});

/* ------------------------------------------------------------------ *
 * Notification
 * ------------------------------------------------------------------ */

test("subscribers hear a moved generation and stay quiet otherwise", () => {
	const module = fakeModule();
	const bridge = createBridge(module);
	let calls = 0;

	bridge.watch("scene.tree");
	bridge.subscribe(() => calls++);

	module.willAnswer(reply({ generations: { scene: 2, selection: 1, history: 1 } }));
	bridge.flush();
	assert.equal(calls, 1);

	module.willAnswer(reply({ generations: { scene: 2, selection: 1, history: 1 } }));
	bridge.flush();
	/* Nothing moved. A notification here would re-render the whole editor
	 * every frame of an idle scene. */
	assert.equal(calls, 1);

	module.willAnswer(reply({ generations: { scene: 2, selection: 4, history: 1 } }));
	bridge.flush();
	assert.equal(calls, 2);
});

test("events wake subscribers even when no generation moved", () => {
	const module = fakeModule();
	const bridge = createBridge(module);
	const heard = [];

	bridge.watch("history");
	bridge.subscribe((r) => heard.push(...r.events));

	module.willAnswer(reply({ generations: { scene: 0, selection: 0, history: 0 } }));
	bridge.flush();
	heard.length = 0;

	module.willAnswer(
		reply({
			generations: { scene: 0, selection: 0, history: 0 },
			events: [{ type: "history.empty", code: 0, text: "nothing to undo" }],
		})
	);
	bridge.flush();
	assert.deepEqual(heard.map((e) => e.type), ["history.empty"]);
});

/* ------------------------------------------------------------------ *
 * Failure
 * ------------------------------------------------------------------ */

test("a tape larger than the engine's buffer is refused, not truncated", () => {
	const module = fakeModule({ capacity: 64 });
	const errors = [];
	const bridge = createBridge(module, { onError: (r) => errors.push(r) });

	for (let i = 0; i < 50; i++) bridge.select(i);
	const result = bridge.flush();

	assert.equal(result.error, "client");
	assert.equal(module.tapes.length, 0, "nothing crossed");
	assert.match(errors[0].events[0].text, /buffer holds 64/);
});

test("an unparseable reply is reported rather than thrown", () => {
	const module = fakeModule();
	module.UTF8ToString = () => "<html>a proxy ate it</html>";
	const bridge = createBridge(module);

	bridge.select(1);
	const result = bridge.flush();
	assert.equal(result.error, "client");
	assert.match(result.events[0].text, /did not parse/);
});

test("an unknown query kind fails at the call, not on the wire", () => {
	const bridge = createBridge(fakeModule());
	assert.throws(() => bridge.watch("scene.trees"), /unknown query kind/);
});

test("a protocol mismatch is visible before anything is driven", () => {
	const ok = createBridge(fakeModule());
	const stale = createBridge(fakeModule({ protocol: BRIDGE_PROTOCOL + 1 }));

	assert.equal(protocolMatches(ok), true);
	assert.equal(protocolMatches(stale), false);
});

test("a module with no bridge exports degrades instead of crashing", () => {
	/* An engine built before #945, or a boot that has not finished. */
	const bare = {
		HEAPU8: new Uint8Array(16),
		UTF8ToString: () => "",
		_krudd_bridge_exchange: () => 0,
	};
	const bridge = createBridge(bare);

	assert.equal(bridge.protocol, -1);
	assert.equal(protocolMatches(bridge), false);
	bridge.select(1);
	assert.equal(bridge.flush().error, "client");
});

test("the heap view is re-read on every flush", () => {
	/*
	 * ALLOW_MEMORY_GROWTH detaches HEAPU8 whenever the engine allocates, so
	 * a client that captured the view at construction would write into freed
	 * memory. Swapping the array between flushes proves it is not captured.
	 */
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.select(1);
	bridge.flush();

	const grown = new Uint8Array(module.HEAPU8.length * 2);
	module.HEAPU8 = grown;
	bridge.select(2);
	bridge.flush();

	assert.equal(module.tapes.length, 2);
	assert.equal(payloadReader(module.tapes[1][0].payload).i32(), 2);
});

/* ------------------------------------------------------------------ *
 * One-shot queries, and the document
 * ------------------------------------------------------------------ */

test("loadScene writes the form as a length-prefixed string", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.loadScene("(scene a (entity))");
	bridge.flush();

	const [record] = module.tapes[0];
	assert.equal(record.op, OP.SCENE_LOAD);
	assert.equal(payloadReader(record.payload).str(), "(scene a (entity))");
});

test("ask sends the query with generation 0 and resolves with the value", async () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	module.willAnswer(
		reply({
			results: [
				{
					kind: "scene.text",
					generation: 4,
					fresh: false,
					value: "(scene saved)",
				},
			],
		})
	);

	const answer = bridge.ask("scene.text");
	bridge.flush();

	const [record] = module.tapes[0];
	assert.equal(record.op, OP.QUERY_SCENE_TEXT);
	/*
	 * 0, never the generation we hold. A one-shot wants the answer, not
	 * confirmation that a cache it does not keep is still good.
	 */
	assert.equal(payloadReader(record.payload).u32(), 0);
	assert.equal(await answer, "(scene saved)");
});

test("a one-shot answer is not left in the cache", async () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	module.willAnswer(
		reply({
			results: [
				{ kind: "scene.text", generation: 1, fresh: false, value: "(scene x)" },
			],
		})
	);
	const answer = bridge.ask("scene.text");
	bridge.flush();
	await answer;

	/*
	 * Nothing watches scene.text, so a lingering entry would be a whole
	 * scene from whenever the last save happened, handed to whoever read
	 * next as though it were current.
	 */
	assert.equal(bridge.read("scene.text"), null);
});

test("watched queries keep their cache while one-shots ride along", async () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.watch("selection");
	module.willAnswer(
		reply({
			results: [
				{ kind: "selection", generation: 2, fresh: false, value: { id: 7 } },
				{ kind: "scene.text", generation: 2, fresh: false, value: "(scene y)" },
			],
		})
	);
	const answer = bridge.ask("scene.text");
	bridge.flush();

	assert.equal(await answer, "(scene y)");
	/* The watched one survives; the one-shot does not. */
	assert.deepEqual(bridge.read("selection"), { id: 7 });
	assert.equal(bridge.read("scene.text"), null);
});

test("the watched set is written before the one-shots", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.watch("history");
	/* Nothing answers it here; the ordering of the tape is the assertion. */
	bridge.ask("scene.text").catch(() => {});
	bridge.flush();

	assert.deepEqual(
		module.tapes[0].map((r) => r.op),
		[OP.QUERY_HISTORY, OP.QUERY_SCENE_TEXT]
	);
});

test("two asks in one frame are two records and two answers", async () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	module.willAnswer(
		reply({
			results: [
				{ kind: "scene.text", generation: 1, fresh: false, value: "first" },
				{ kind: "scene.text", generation: 1, fresh: false, value: "second" },
			],
		})
	);
	const a = bridge.ask("scene.text");
	const b = bridge.ask("scene.text");
	bridge.flush();

	assert.equal(module.tapes[0].length, 2);
	assert.deepEqual([await a, await b], ["first", "second"]);
});

test("an ask made while the reply is handled belongs to the next flush", async () => {
	const module = fakeModule();
	const bridge = createBridge(module);
	let second = null;

	bridge.subscribe(() => {
		second ??= bridge.ask("scene.text");
	});
	module.willAnswer(
		reply({
			events: [{ type: "log", code: 0, text: "hi" }],
			results: [
				{ kind: "scene.text", generation: 1, fresh: false, value: "one" },
			],
		})
	);
	const first = bridge.ask("scene.text");
	bridge.flush();

	assert.equal(await first, "one");
	assert.equal(module.tapes.length, 1);

	module.willAnswer(
		reply({
			results: [
				{ kind: "scene.text", generation: 2, fresh: false, value: "two" },
			],
		})
	);
	bridge.flush();
	assert.equal(await second, "two");
});

test("a null answer resolves as null rather than rejecting", async () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	/*
	 * The engine's way of saying it could not write a scene. It is an
	 * answer, not a transport failure, and the caller distinguishes them.
	 */
	module.willAnswer(
		reply({
			results: [
				{ kind: "scene.text", generation: 1, fresh: false, value: null },
			],
		})
	);
	const answer = bridge.ask("scene.text");
	bridge.flush();

	assert.equal(await answer, null);
});

test("an ask the engine did not answer rejects", async () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	module.willAnswer(reply({ results: [] }));
	const answer = bridge.ask("scene.text");
	bridge.flush();

	await assert.rejects(answer, /did not answer scene\.text/);
});

test("an ask on a flush that never crossed rejects", async () => {
	const module = fakeModule({ capacity: 16 });
	const bridge = createBridge(module);

	/* Too big for the engine's buffer, so the whole tape is dropped. */
	bridge.loadScene("x".repeat(64));
	const answer = bridge.ask("scene.text");
	bridge.flush();

	await assert.rejects(answer, /bytes and the engine's buffer holds/);
});

test("an ask alone is enough to make a flush happen", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	assert.ok(bridge.idle);
	bridge.ask("scene.text").catch(() => {});
	assert.ok(!bridge.idle);
	assert.ok(bridge.flush() !== null);
});

test("ask refuses a query kind that does not exist", () => {
	const bridge = createBridge(fakeModule());
	assert.throws(() => bridge.ask("nonsense"), /unknown query kind/);
});

/* ------------------------------------------------------------------ *
 * Editable parameters
 * ------------------------------------------------------------------ */

test("setParam writes id, slot, field and four components", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.setParam(7, 1, 2, [0.5, 0.25]);
	bridge.flush();

	const [record] = module.tapes[0];
	assert.equal(record.op, OP.ENTITY_PARAM);
	/*
	 * Fixed size regardless of the field's arity — that is what lets the C
	 * side check the whole tape before applying any of it.
	 */
	assert.equal(record.payload.length, 4 + 4 + 4 + 16);

	const r = payloadReader(record.payload);
	assert.deepEqual([r.i32(), r.i32(), r.i32()], [7, 1, 2]);
	/* Missing components go out as 0 rather than as undefined-turned-NaN. */
	assert.deepEqual([r.f32(), r.f32(), r.f32(), r.f32()], [0.5, 0.25, 0, 0]);
	assert.ok(r.done);
});

test("entity.params is keyed by entity, so two entities cache separately", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.watch("entity.params", 3);
	bridge.watch("entity.params", 4);
	module.willAnswer(
		reply({
			results: [
				{
					kind: "entity.params",
					id: 3,
					generation: 1,
					fresh: false,
					value: { id: 3, blocks: [] },
				},
				{
					kind: "entity.params",
					id: 4,
					generation: 1,
					fresh: false,
					value: { id: 4, blocks: [{ slot: "mesh" }] },
				},
			],
		})
	);
	bridge.flush();

	assert.deepEqual(bridge.read("entity.params", 3), { id: 3, blocks: [] });
	assert.equal(bridge.read("entity.params", 4).blocks[0].slot, "mesh");
});

/* ------------------------------------------------------------------ *
 * The viewport (#949)
 *
 * The encoder half of the domain the C suite tests from the other side. What
 * matters here is that each command lands on the right opcode with the right
 * payload, because the C decoder's `op_fixed_bytes` rejects a whole batch on a
 * length mismatch — so a wrong arity here is not one broken command, it is a
 * frame in which nothing the editor did reached the engine.
 * ------------------------------------------------------------------ */

test("the camera commands carry their gestures and nothing else", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.setViewportSize(1280, 720);
	bridge.orbitCamera(0.25, -0.5);
	bridge.panCamera(0.1, 0.2);
	bridge.dollyCamera(-0.3);
	bridge.frameCamera();
	bridge.resetCamera();
	bridge.flush();

	const [tape] = module.tapes;
	assert.deepEqual(
		tape.map((r) => r.op),
		[
			OP.VIEWPORT_SIZE,
			OP.CAMERA_ORBIT,
			OP.CAMERA_PAN,
			OP.CAMERA_DOLLY,
			OP.CAMERA_FRAME,
			OP.CAMERA_RESET,
		]
	);

	const size = payloadReader(tape[0].payload);
	assert.deepEqual([size.f32(), size.f32()], [1280, 720]);
	assert.ok(size.done);

	const orbit = payloadReader(tape[1].payload);
	assert.equal(orbit.f32(), 0.25);
	assert.equal(orbit.f32(), -0.5);
	assert.ok(orbit.done);

	const dolly = payloadReader(tape[3].payload);
	assert.equal(dolly.f32().toFixed(3), "-0.300");
	assert.ok(dolly.done);

	/* Frame with no argument means the selection, so a shortcut cannot
	 * disagree with the engine about what is selected. */
	assert.equal(payloadReader(tape[4].payload).i32(), -1);
	assert.equal(tape[5].payload.length, 0);
});

test("a pick carries the pixel it was asked at", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.pick(400.5, 300.25);
	bridge.flush();

	const [[record]] = module.tapes;
	assert.equal(record.op, OP.PICK);
	const r = payloadReader(record.payload);
	assert.deepEqual([r.f32(), r.f32()], [400.5, 300.25]);
	assert.ok(r.done);
});

test("editor mode goes across as an int, not a bool", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.setEditorMode(true);
	bridge.setEditorMode(false);
	bridge.flush();

	const [tape] = module.tapes;
	assert.equal(payloadReader(tape[0].payload).i32(), 1);
	assert.equal(payloadReader(tape[1].payload).i32(), 0);
});

test("the gizmo commands carry mode, snap, drag and grid", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.setGizmoMode(GIZMO_MODE.ROTATE);
	bridge.setGizmoSnap(0.5, 15, 0.25);
	bridge.gizmoDrag(DRAG_PHASE.BEGIN, 10, 20);
	bridge.setGrid(false, 2);
	bridge.flush();

	const [tape] = module.tapes;
	assert.equal(payloadReader(tape[0].payload).i32(), GIZMO_MODE.ROTATE);

	const snap = payloadReader(tape[1].payload);
	assert.deepEqual([snap.f32(), snap.f32(), snap.f32()], [0.5, 15, 0.25]);
	assert.ok(snap.done);

	const drag = payloadReader(tape[2].payload);
	assert.equal(drag.i32(), DRAG_PHASE.BEGIN);
	assert.deepEqual([drag.f32(), drag.f32()], [10, 20]);
	assert.ok(drag.done);

	const grid = payloadReader(tape[3].payload);
	assert.equal(grid.i32(), 0);
	assert.equal(grid.f32(), 2);
	assert.ok(grid.done);
});

test("the viewport query is cached against its own generation", () => {
	const module = fakeModule();
	const bridge = createBridge(module);
	const state = {
		width: 800,
		height: 600,
		editorMode: true,
		mode: GIZMO_MODE.TRANSLATE,
		axis: -1,
		dragSerial: 3,
		snap: { translate: 0, rotate: 0, scale: 0 },
		grid: { shown: true, spacing: 1 },
	};

	bridge.watch("viewport");
	module.willAnswer(
		reply({
			generations: { scene: 1, selection: 1, history: 1, viewport: 7 },
			results: [
				{ kind: "viewport", generation: 7, fresh: false, value: state },
			],
		})
	);
	bridge.flush();
	assert.deepEqual(bridge.read("viewport"), state);

	/* The next flush sends the generation it now holds, and a `fresh` answer
	 * leaves the held value alone rather than replacing it with nothing. */
	module.willAnswer(
		reply({
			generations: { scene: 2, selection: 1, history: 1, viewport: 7 },
			results: [{ kind: "viewport", generation: 7, fresh: true }],
		})
	);
	bridge.flush();
	assert.equal(payloadReader(module.tapes[1][0].payload).u32(), 7);
	assert.deepEqual(bridge.read("viewport"), state);
});

test("a viewport generation moving on its own still notifies", () => {
	const module = fakeModule();
	const bridge = createBridge(module);
	let heard = 0;

	bridge.subscribe(() => {
		heard += 1;
	});
	bridge.watch("viewport");

	/*
	 * Nothing else moved — this is a reader pressing a mode button while
	 * the scene sits still. A client that only watched the first three
	 * domains would render the old mode until something else changed.
	 */
	module.willAnswer(
		reply({
			generations: { scene: 1, selection: 1, history: 1, viewport: 2 },
			results: [],
		})
	);
	bridge.flush();
	assert.equal(heard, 1);
});

/* ------------------------------------------------------------------ *
 * The outliner (#950)
 * ------------------------------------------------------------------ */

test("the selection modifiers are three opcodes, not a flag on select", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.select(1);
	bridge.selectAdd(4);
	bridge.selectRemove(1);
	bridge.selectClear();
	bridge.flush();

	const [tape] = module.tapes;
	assert.deepEqual(
		tape.map((r) => r.op),
		[OP.SELECT, OP.SELECT_ADD, OP.SELECT_REMOVE, OP.SELECT_CLEAR]
	);
	assert.equal(payloadReader(tape[1].payload).i32(), 4);
	/* Clear carries nothing — there is no id to clear to. */
	assert.equal(tape[3].payload.length, 0);
});

test("reparenting to the root is -1, not a sentinel of its own", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.setParent(3, 7);
	bridge.setParent(3, -1);
	bridge.flush();

	const [tape] = module.tapes;
	assert.deepEqual(
		tape.map((r) => r.op),
		[OP.ENTITY_PARENT, OP.ENTITY_PARENT]
	);

	const first = payloadReader(tape[0].payload);
	assert.equal(first.i32(), 3);
	assert.equal(first.i32(), 7);

	const second = payloadReader(tape[1].payload);
	assert.equal(second.i32(), 3);
	assert.equal(second.i32(), -1);
});

test("duplicate carries the id and nothing else", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.duplicateEntity(9);
	bridge.flush();

	const [tape] = module.tapes;
	assert.deepEqual(
		tape.map((r) => r.op),
		[OP.ENTITY_DUPLICATE]
	);
	assert.equal(payloadReader(tape[0].payload).i32(), 9);
});

test("a selection answer carries the whole set", () => {
	const module = fakeModule();
	const bridge = createBridge(module);

	bridge.watch("selection");
	module.willAnswer(
		reply({
			generations: { scene: 0, selection: 3, history: 0, viewport: 0 },
			results: [
				{
					kind: "selection",
					generation: 3,
					fresh: false,
					value: { id: 4, ids: [2, 4] },
				},
			],
		})
	);
	bridge.flush();

	assert.deepEqual(bridge.read("selection"), { id: 4, ids: [2, 4] });
});
