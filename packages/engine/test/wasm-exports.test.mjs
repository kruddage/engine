// SPDX-License-Identifier: GPL-2.0-or-later
//
// The export reader is checked against modules built here, byte by byte, rather
// than against a fixture emcc produced — the point is to pin the decoding to the
// spec, not to whatever one toolchain version happened to emit.

import assert from "node:assert/strict";
import { test } from "node:test";

import { readWasmExports } from "../src/wasm-exports.mjs";

/* LEB128, unsigned. */
function varuint(n) {
	const bytes = [];
	do {
		let byte = n & 0x7f;
		n >>>= 7;
		if (n !== 0) byte |= 0x80;
		bytes.push(byte);
	} while (n !== 0);
	return bytes;
}

function section(id, payload) {
	return [id, ...varuint(payload.length), ...payload];
}

function exportSection(entries) {
	const payload = [...varuint(entries.length)];
	for (const [name, kind, index] of entries) {
		const encoded = [...new TextEncoder().encode(name)];
		payload.push(...varuint(encoded.length), ...encoded, kind, ...varuint(index));
	}
	return section(7, payload);
}

const HEADER = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00];

function moduleOf(...sections) {
	return new Uint8Array([...HEADER, ...sections.flat()]);
}

test("reads function exports in module order", () => {
	const wasm = moduleOf(
		exportSection([
			["main", 0, 3],
			["krudd_load_game", 0, 17],
			["memory", 2, 0],
		])
	);

	assert.deepEqual(readWasmExports(wasm), [
		{ name: "main", kind: "function", index: 3 },
		{ name: "krudd_load_game", kind: "function", index: 17 },
		{ name: "memory", kind: "memory", index: 0 },
	]);
});

test("skips sections that precede the export section", () => {
	/* A custom section (id 0) carrying bytes that would decode as nonsense if
	 * the reader tried to interpret them. */
	const custom = section(0, [0x04, 0x6e, 0x61, 0x6d, 0x65, 0xff, 0xff, 0xff]);
	const wasm = moduleOf(custom, exportSection([["only", 0, 1]]));

	assert.deepEqual(readWasmExports(wasm), [
		{ name: "only", kind: "function", index: 1 },
	]);
});

test("a module with no export section exports nothing", () => {
	assert.deepEqual(readWasmExports(moduleOf(section(0, [0x00]))), []);
});

test("multi-byte LEB128 lengths and indices decode", () => {
	const name = "x".repeat(200);
	const wasm = moduleOf(exportSection([[name, 0, 300]]));

	assert.deepEqual(readWasmExports(wasm), [
		{ name, kind: "function", index: 300 },
	]);
});

test("rejects a file that is not WASM", () => {
	assert.throws(
		() => readWasmExports(new TextEncoder().encode("<!doctype html>")),
		/bad magic/
	);
});

test("rejects a section that runs past the end of the module", () => {
	const wasm = new Uint8Array([...HEADER, 7, 0x7f, 0x01]);
	assert.throws(() => readWasmExports(wasm), /past end/);
});
