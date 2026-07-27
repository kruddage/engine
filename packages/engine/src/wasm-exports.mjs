// SPDX-License-Identifier: GPL-2.0-or-later
//
// Reads the export section of a WASM module.
//
// The point is that @kruddage/engine's published API surface is *derived from
// the artifact* rather than asserted alongside it. ninja.scm passes
// -sEXPORTED_FUNCTIONS to emcc; this reads back what actually came out, so the
// manifest describes the module that exists instead of the one the build flags
// intended. Only the export section is decoded — enough to name the surface,
// and nothing that would make this a WASM parser to maintain.
//
// Format: https://webassembly.github.io/spec/core/binary/modules.html

const MAGIC = 0x6d736100; /* "\0asm", little-endian */
const SECTION_EXPORT = 7;

export const EXPORT_KINDS = ["function", "table", "memory", "global"];

/* LEB128, unsigned. Returns the value and the offset just past it. */
function readVarUint(bytes, offset) {
	let result = 0;
	let shift = 0;
	let pos = offset;

	for (;;) {
		if (pos >= bytes.length) throw new Error("truncated LEB128");
		const byte = bytes[pos++];
		result += (byte & 0x7f) * 2 ** shift;
		if ((byte & 0x80) === 0) return { value: result, offset: pos };
		shift += 7;
		if (shift > 35) throw new Error("LEB128 too long for a 32-bit index");
	}
}

/**
 * Names exported by a WASM module, in module order.
 *
 * @param {Uint8Array} bytes a complete .wasm binary
 * @returns {{name: string, kind: string, index: number}[]}
 */
export function readWasmExports(bytes) {
	const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	if (bytes.length < 8 || view.getUint32(0, true) !== MAGIC) {
		throw new Error("not a WASM module (bad magic)");
	}

	const decoder = new TextDecoder("utf-8", { fatal: true });
	let pos = 8; /* magic + version */

	while (pos < bytes.length) {
		const id = bytes[pos++];
		const size = readVarUint(bytes, pos);
		pos = size.offset;
		const end = pos + size.value;
		if (end > bytes.length) throw new Error("section runs past end of module");

		/* Custom sections (id 0) and everything before the export section are
		 * skipped wholesale — their contents are irrelevant here and decoding
		 * them would be the parser this deliberately is not. */
		if (id !== SECTION_EXPORT) {
			pos = end;
			continue;
		}

		const count = readVarUint(bytes, pos);
		pos = count.offset;

		const exports = [];
		for (let i = 0; i < count.value; i++) {
			const len = readVarUint(bytes, pos);
			pos = len.offset;
			const name = decoder.decode(bytes.subarray(pos, pos + len.value));
			pos += len.value;

			const kind = bytes[pos++];
			const index = readVarUint(bytes, pos);
			pos = index.offset;

			exports.push({
				name,
				kind: EXPORT_KINDS[kind] ?? `unknown(${kind})`,
				index: index.value,
			});
		}
		return exports;
	}

	/* A module with no export section exports nothing — not an error. */
	return [];
}
