// SPDX-License-Identifier: GPL-2.0-or-later
//
// @kruddage/engine/bridge — the editor boundary's client half (#945).
//
// The engine owns the document; this is how something else reads and edits it.
// One crossing per frame: commands and queries go out as a binary tape, one
// JSON document comes back with the generation vector, any notices, and the
// answer to every query in the tape. The C half is krudd/engine/ui/bridge, and
// its header carries the reasoning for the shape.
//
// ## Why this lives here and not in @kruddage/editor
//
// The boundary is not React's, and it is not the editor's. A game shell will
// want to drive the engine from JS and will never want a UI framework, so the
// client belongs to the package that already owns the engine's JS-facing API
// (#944's Q4 decision comment says exactly this).
//
// It is reachable only as `@kruddage/engine/bridge`, never from the package
// root. The root entry point imports node:fs and node:path — it is build-time
// code — and pulling that into a browser bundle to get at this would be a
// mistake nothing else would catch. Nothing in this file imports a Node
// builtin, and nothing in it may.
//
// ## What this is not
//
// **It is not a store.** It holds a cache of what the engine last said, keyed
// by generation, and it throws that cache away the moment the engine says the
// generation moved. Nothing here is authoritative, nothing here is edited
// locally, and there is deliberately no way to write to it: `apply()` queues a
// command for the engine, and the value changes when — and only when — the
// engine says it did (#944, Q1/Q2).
//
// **It is not an undo stack.** undo() and redo() are commands like any other.
// The 128-entry ring in world/edit is the only history in the system.

/** The wire version this client speaks. Must match the module's. */
export const BRIDGE_PROTOCOL = 1;

/** 'K' 'B' 'R' 'G', little-endian. */
export const TAPE_MAGIC = 0x47524248;

/*
 * Opcodes, mirroring enum bridge_op. Grouped, sparse, and permanent: an opcode
 * is never reused for a different meaning, because a stale bundle in someone's
 * cache would then drive the wrong mutator instead of failing.
 */
export const OP = Object.freeze({
	GESTURE_BEGIN: 0x0001,
	GESTURE_COMMIT: 0x0002,
	GESTURE_ABORT: 0x0003,
	UNDO: 0x0010,
	REDO: 0x0011,
	SELECT: 0x0020,
	ENTITY_CREATE: 0x0030,
	ENTITY_DESTROY: 0x0031,
	ENTITY_TRANSFORM: 0x0032,
	ENTITY_NAME: 0x0033,
	ENTITY_RENDER_REF: 0x0034,
	ENTITY_MATERIAL_REF: 0x0035,
	ENTITY_SCRIPT_REF: 0x0036,
	SET_PAUSED: 0x0040,
	QUERY_TREE: 0x0100,
	QUERY_ENTITY: 0x0101,
	QUERY_SELECTION: 0x0102,
	QUERY_HISTORY: 0x0103,
});

/** Query kinds, and the domain whose generation each is cached against. */
const QUERY = Object.freeze({
	"scene.tree": { op: OP.QUERY_TREE, domain: "scene", keyed: false },
	entity: { op: OP.QUERY_ENTITY, domain: "scene", keyed: true },
	selection: { op: OP.QUERY_SELECTION, domain: "selection", keyed: false },
	history: { op: OP.QUERY_HISTORY, domain: "history", keyed: false },
});

const DEFAULT_GENERATIONS = Object.freeze({
	scene: 0,
	selection: 0,
	history: 0,
});

const TRANSFORM_FLOATS = 10;

/**
 * The tape encoder.
 *
 * Grows by doubling rather than by exact fit — a batch is written once a frame
 * and thrown away, so the allocation that matters is the one that does not
 * happen on the steady-state frame where the buffer is already big enough.
 *
 * Every multi-byte value is written little-endian **explicitly**. The host's
 * endianness is not consulted: the C side rejects a tape whose magic reads
 * backwards, and relying on the platform to agree would be a bug that only
 * appears on hardware nobody tests on.
 */
export class Tape {
	#bytes;
	#view;
	#len = 0;
	#count = 0;
	#record = -1;

	constructor(capacity = 4096) {
		this.#bytes = new Uint8Array(capacity);
		this.#view = new DataView(this.#bytes.buffer);
		this.reset();
	}

	/** Bytes written so far, header included. */
	get length() {
		return this.#len;
	}

	/** Records closed so far. */
	get count() {
		return this.#count;
	}

	reset() {
		this.#len = 0;
		this.#count = 0;
		this.#record = -1;
		this.#need(8);
		this.#view.setUint32(0, TAPE_MAGIC, true);
		this.#view.setUint32(4, 0, true);
		this.#len = 8;
		return this;
	}

	#need(extra) {
		if (this.#len + extra <= this.#bytes.length) return;
		let size = this.#bytes.length || 64;
		while (size < this.#len + extra) size *= 2;
		const grown = new Uint8Array(size);
		grown.set(this.#bytes.subarray(0, this.#len));
		this.#bytes = grown;
		this.#view = new DataView(grown.buffer);
	}

	open(op) {
		this.#need(4);
		this.#view.setUint16(this.#len, op, true);
		this.#record = this.#len + 2;
		this.#view.setUint16(this.#record, 0, true);
		this.#len += 4;
		return this;
	}

	close() {
		this.#view.setUint16(this.#record, this.#len - this.#record - 2, true);
		this.#count += 1;
		this.#view.setUint32(4, this.#count, true);
		return this;
	}

	i32(value) {
		this.#need(4);
		this.#view.setInt32(this.#len, value | 0, true);
		this.#len += 4;
		return this;
	}

	u32(value) {
		this.#need(4);
		this.#view.setUint32(this.#len, value >>> 0, true);
		this.#len += 4;
		return this;
	}

	f32(value) {
		this.#need(4);
		this.#view.setFloat32(this.#len, value, true);
		this.#len += 4;
		return this;
	}

	/**
	 * A transform, as ten floats: position xyz, rotation xyzw, scale xyz.
	 *
	 * Missing components fall back to identity rather than to zero. A scale
	 * of zero collapses an entity to nothing, and "the caller left `scale`
	 * off" should not be the way that happens.
	 */
	transform(t) {
		const p = t?.position ?? [0, 0, 0];
		const r = t?.rotation ?? [0, 0, 0, 1];
		const s = t?.scale ?? [1, 1, 1];
		this.f32(p[0] ?? 0).f32(p[1] ?? 0).f32(p[2] ?? 0);
		this.f32(r[0] ?? 0).f32(r[1] ?? 0).f32(r[2] ?? 0).f32(r[3] ?? 1);
		this.f32(s[0] ?? 1).f32(s[1] ?? 1).f32(s[2] ?? 1);
		return this;
	}

	/** A u16-length-prefixed UTF-8 string. */
	str(value) {
		const encoded = ENCODER.encode(String(value ?? ""));
		this.#need(2 + encoded.length);
		this.#view.setUint16(this.#len, encoded.length, true);
		this.#len += 2;
		this.#bytes.set(encoded, this.#len);
		this.#len += encoded.length;
		return this;
	}

	/** The written bytes. A view, not a copy — valid until the next write. */
	bytes() {
		return this.#bytes.subarray(0, this.#len);
	}
}

const ENCODER = new TextEncoder();

/**
 * Wrap an emscripten module in the boundary's client.
 *
 * `module` needs the four `_krudd_bridge_*` exports, `HEAPU8`, and
 * `UTF8ToString`. Nothing else about it is touched.
 */
export function createBridge(module, options = {}) {
	const onError = options.onError ?? null;
	const tape = new Tape();

	/*
	 * Queued between flushes. Commands are ordered and replayed exactly as
	 * written; queries are a set, because asking for the same thing twice in
	 * one frame is a caller mistake rather than a request for two answers.
	 */
	let pending = [];
	const watched = new Map();

	const cache = new Map();
	let generations = { ...DEFAULT_GENERATIONS };
	let lastReply = null;
	const listeners = new Set();

	const protocol = call(module, "_krudd_bridge_protocol", -1);
	const capacity = call(module, "_krudd_bridge_capacity", 0);

	function queryKey(kind, id) {
		return QUERY[kind]?.keyed ? `${kind}:${id}` : kind;
	}

	/**
	 * Register interest in a query. It is re-asked on every flush — cheaply,
	 * because the engine answers `fresh` and sends nothing when the
	 * generation has not moved.
	 */
	function watch(kind, id = -1) {
		if (!QUERY[kind]) throw new Error(`unknown query kind: ${kind}`);
		const key = queryKey(kind, id);
		const entry = watched.get(key);
		if (entry) {
			entry.refs += 1;
		} else {
			watched.set(key, { kind, id, refs: 1 });
		}
		return () => {
			const live = watched.get(key);
			if (!live) return;
			live.refs -= 1;
			if (live.refs <= 0) {
				watched.delete(key);
				/*
				 * The cached value goes with the last watcher. Keeping
				 * it would mean a later watcher reads a value from
				 * before an unwatched interval, with a generation that
				 * claims it is current.
				 */
				cache.delete(key);
			}
		};
	}

	/** The last value the engine gave for a query, or null. */
	function read(kind, id = -1) {
		const entry = cache.get(queryKey(kind, id));
		return entry ? entry.value : null;
	}

	function queue(build) {
		pending.push(build);
		return api;
	}

	/*
	 * Every mutator is a queued tape record and nothing else — no local
	 * state changes here, ever. That is the rule that makes the UI a view:
	 * the value moves when the engine says it moved, one flush later, and
	 * never because the caller asked for it.
	 */
	const api = {
		get protocol() {
			return protocol;
		},
		get capacity() {
			return capacity;
		},
		get generations() {
			return { ...generations };
		},
		get lastReply() {
			return lastReply;
		},

		watch,
		read,

		beginGesture: (label) =>
			queue((t) => t.open(OP.GESTURE_BEGIN).str(label).close()),
		commitGesture: () => queue((t) => t.open(OP.GESTURE_COMMIT).close()),
		abortGesture: () => queue((t) => t.open(OP.GESTURE_ABORT).close()),

		undo: () => queue((t) => t.open(OP.UNDO).close()),
		redo: () => queue((t) => t.open(OP.REDO).close()),

		select: (id) => queue((t) => t.open(OP.SELECT).i32(id).close()),
		setPaused: (paused) =>
			queue((t) => t.open(OP.SET_PAUSED).i32(paused ? 1 : 0).close()),

		createEntity: (parent, transform, mask = 0, renderRef = 0) =>
			queue((t) =>
				t
					.open(OP.ENTITY_CREATE)
					.i32(parent)
					.transform(transform)
					.u32(mask)
					.u32(renderRef)
					.close()
			),
		destroyEntity: (id) =>
			queue((t) => t.open(OP.ENTITY_DESTROY).i32(id).close()),
		setTransform: (id, transform) =>
			queue((t) =>
				t.open(OP.ENTITY_TRANSFORM).i32(id).transform(transform).close()
			),
		setName: (id, name) =>
			queue((t) => t.open(OP.ENTITY_NAME).i32(id).str(name).close()),
		setRenderRef: (id, ref) =>
			queue((t) =>
				t.open(OP.ENTITY_RENDER_REF).i32(id).u32(ref).close()
			),
		setMaterialRef: (id, ref) =>
			queue((t) =>
				t.open(OP.ENTITY_MATERIAL_REF).i32(id).u32(ref).close()
			),
		setScriptRef: (id, ref) =>
			queue((t) =>
				t.open(OP.ENTITY_SCRIPT_REF).i32(id).u32(ref).close()
			),

		/** Called after every flush that changed a generation or carried events. */
		subscribe(listener) {
			listeners.add(listener);
			return () => listeners.delete(listener);
		},

		/** Whether anything is waiting to go out. */
		get idle() {
			return pending.length === 0 && watched.size === 0;
		},

		flush,
		encode,
	};

	/** Build the tape for the currently queued work. Exposed for tests. */
	function encode() {
		tape.reset();
		for (const build of pending) build(tape);
		for (const { kind, id } of watched.values()) {
			const spec = QUERY[kind];
			const known = cache.get(queryKey(kind, id))?.generation ?? 0;
			tape.open(spec.op);
			if (spec.keyed) tape.i32(id);
			tape.u32(known);
			tape.close();
		}
		return tape;
	}

	/**
	 * One crossing.
	 *
	 * Returns the parsed reply, or null when there was nothing to say and
	 * nothing to ask — a page with no editor attached must not pay a call a
	 * frame to be told the same thing.
	 */
	function flush() {
		if (pending.length === 0 && watched.size === 0) return null;

		encode();
		const bytes = tape.bytes();
		if (bytes.length > capacity) {
			/*
			 * Dropped rather than truncated: half a batch is worse than
			 * none, and the commands stay queued so the caller can split
			 * them across frames instead of losing them.
			 */
			return fail(
				`tape is ${bytes.length} bytes and the engine's buffer holds ${capacity}`
			);
		}

		const at = call(module, "_krudd_bridge_buffer", 0);
		if (!at) return fail("the engine exposes no bridge buffer");

		/*
		 * HEAPU8 is re-read here, every flush, and must never be hoisted:
		 * the module is linked with ALLOW_MEMORY_GROWTH, so any allocation
		 * anywhere in the engine can detach the old view and leave a cached
		 * one pointing at freed memory.
		 */
		module.HEAPU8.set(bytes, at);

		const ptr = module._krudd_bridge_exchange(bytes.length);
		const text = module.UTF8ToString(ptr);

		let reply;
		try {
			reply = JSON.parse(text);
		} catch {
			return fail(`the engine's reply did not parse: ${text.slice(0, 120)}`);
		}

		/* The batch is gone whether or not the engine liked it — resending
		 * commands the engine may have applied would double them. */
		pending = [];
		return accept(reply);
	}

	function accept(reply) {
		const before = generations;
		generations = { ...DEFAULT_GENERATIONS, ...(reply.generations ?? {}) };

		for (const result of reply.results ?? []) {
			const key = queryKey(result.kind, result.id ?? -1);
			if (result.fresh) {
				/* A hit. Restamp what we hold; the value is unchanged. */
				const held = cache.get(key);
				if (held) held.generation = result.generation;
				continue;
			}
			cache.set(key, {
				generation: result.generation,
				value: result.value ?? null,
			});
		}

		lastReply = reply;
		if (reply.error && onError) onError(reply);

		const moved =
			before.scene !== generations.scene ||
			before.selection !== generations.selection ||
			before.history !== generations.history;
		if (moved || (reply.events?.length ?? 0) > 0 || reply.error) {
			for (const listener of listeners) listener(reply);
		}
		return reply;
	}

	/*
	 * A client-side failure wearing the same shape as an engine-side one, so
	 * a caller has one thing to check rather than two. `code` is 0 because
	 * the engine's codes are the engine's; this never reached it.
	 */
	function fail(message) {
		const reply = {
			protocol,
			serial: lastReply?.serial ?? 0,
			applied: 0,
			error: "client",
			code: 0,
			generations: { ...generations },
			events: [{ type: "bridge.client", code: 0, text: message }],
			eventsDropped: 0,
			results: [],
		};
		lastReply = reply;
		if (onError) onError(reply);
		for (const listener of listeners) listener(reply);
		return reply;
	}

	return api;
}

function call(module, name, fallback) {
	const fn = module?.[name];
	return typeof fn === "function" ? fn() : fallback;
}

/**
 * Whether a module speaks a protocol this client understands.
 *
 * Checked rather than assumed: the editor and the engine are built and cached
 * separately, so a browser can hold a bundle from one commit and a wasm module
 * from another. Failing loudly at boot beats decoding a tape the other side
 * writes differently.
 */
export function protocolMatches(bridge) {
	return bridge.protocol === BRIDGE_PROTOCOL;
}
