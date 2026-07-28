// SPDX-License-Identifier: GPL-2.0-or-later
//
// A wasm module that isn't one, so the document can be tested against the real
// bridge client rather than a stand-in for it.
//
// Stubbing the *module* rather than the Bridge is the whole point. The document
// layer's job is to drive the boundary correctly — to open a gesture around a
// drag, to ask once rather than watch, to know when a load has landed — and a
// hand-written fake Bridge would let all of that be wrong in exactly the way
// the fake was written to expect. Here the tape is really encoded, really
// decoded, and the reply really parsed.
//
// What is faked is the engine's behaviour, and only as far as each test needs.

import {
	BRIDGE_PROTOCOL,
	OP,
	TAPE_MAGIC,
	type BridgeModule,
	type BridgeReply,
	type BridgeResult,
} from "@kruddage/engine/bridge";

const BUFFER_AT = 1024;

export interface TapeRecord {
	op: number;
	payload: Uint8Array;
}

export interface ModuleStub extends BridgeModule {
	/** Queue what the next exchange answers with. */
	willAnswer: (reply: Partial<BridgeReply>) => void;
	/** The records decoded from each exchange so far. */
	readonly tapes: TapeRecord[][];
	/** Every op sent, flattened — the usual shape an assertion wants. */
	readonly ops: number[];
}

export function reply(overrides: Partial<BridgeReply> = {}): BridgeReply {
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

export function moduleStub(protocol = BRIDGE_PROTOCOL): ModuleStub {
	const heap = new Uint8Array(BUFFER_AT + 64 * 1024 + 4096);
	const queued: Partial<BridgeReply>[] = [];
	const tapes: TapeRecord[][] = [];
	let text = "";

	return {
		HEAPU8: heap,
		UTF8ToString: () => text,
		_krudd_bridge_protocol: () => protocol,
		_krudd_bridge_buffer: () => BUFFER_AT,
		_krudd_bridge_capacity: () => 64 * 1024,
		_krudd_bridge_exchange(length: number) {
			const records = decode(heap.subarray(BUFFER_AT, BUFFER_AT + length));
			tapes.push(records);
			const overrides = queued.shift() ?? {};
			/*
			 * Every query in the tape gets a result, in tape order. That is the
			 * engine's contract (run_queries walks the tape and answers each),
			 * and a stub that answered fewer would fail one-shots that merely
			 * rode along with something else — which is exactly how load()
			 * waits for its command to land.
			 */
			text = JSON.stringify(
				reply({ results: answer(records), ...overrides })
			);
			return 1;
		},
		willAnswer: (next) => void queued.push(next),
		get tapes() {
			return tapes;
		},
		get ops() {
			return tapes.flat().map((record) => record.op);
		},
	};
}

/*
 * A plausible answer per query op. Values are the shape the C side writes and
 * nothing more — a test that cares about content queues its own results.
 */
const ANSWERS: Record<number, { kind: string; value: unknown }> = {
	[OP.QUERY_TREE]: { kind: "scene.tree", value: { paused: false, entities: [] } },
	[OP.QUERY_ENTITY]: { kind: "entity", value: null },
	[OP.QUERY_SELECTION]: { kind: "selection", value: { id: -1 } },
	[OP.QUERY_HISTORY]: {
		kind: "history",
		value: { canUndo: false, canRedo: false, undoLabel: null, redoLabel: null },
	},
	[OP.QUERY_SCENE_TEXT]: { kind: "scene.text", value: "(scene stub)" },
};

function answer(records: TapeRecord[]): BridgeResult[] {
	return records.flatMap((record) => {
		const spec = ANSWERS[record.op];
		if (!spec) return [];
		return [{ kind: spec.kind, generation: 1, fresh: false, value: spec.value }];
	});
}

/** The C decoder's rules, in TypeScript. Mirrors the engine package's. */
function decode(bytes: Uint8Array): TapeRecord[] {
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	if (view.getUint32(0, true) !== TAPE_MAGIC) throw new Error("bad magic");

	const count = view.getUint32(4, true);
	const records: TapeRecord[] = [];
	let at = 8;
	for (let i = 0; i < count; i++) {
		const op = view.getUint16(at, true);
		const length = view.getUint16(at + 2, true);
		records.push({ op, payload: bytes.subarray(at + 4, at + 4 + length) });
		at += 4 + length;
	}
	return records;
}

/** The string a record carries, after any leading fixed fields. */
export function readString(payload: Uint8Array, offset = 0): string {
	const view = new DataView(
		payload.buffer,
		payload.byteOffset,
		payload.byteLength
	);
	const length = view.getUint16(offset, true);
	return new TextDecoder().decode(
		payload.subarray(offset + 2, offset + 2 + length)
	);
}

/**
 * A scheduler that flushes when the test says so.
 *
 * The document's default is requestAnimationFrame, and a suite that waited for
 * real frames to observe a command would be slow and flaky for no reason worth
 * paying. Every test here drives the loop by hand.
 */
export function manualScheduler(): {
	scheduler: (tick: () => void) => () => void;
	frame: () => void;
	readonly running: boolean;
} {
	let tick: (() => void) | null = null;
	return {
		scheduler: (next) => {
			tick = next;
			return () => {
				tick = null;
			};
		},
		frame: () => tick?.(),
		get running() {
			return tick !== null;
		},
	};
}
