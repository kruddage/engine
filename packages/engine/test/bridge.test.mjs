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
		generations: { scene: 1, selection: 1, history: 1 },
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
