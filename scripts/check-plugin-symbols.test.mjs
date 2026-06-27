// SPDX-License-Identifier: GPL-2.0-or-later
//
// Self-test for check-plugin-symbols.mjs. It builds tiny synthetic WASM modules
// in memory (no emcc required) and asserts the reconciliation flags exactly the
// imports that nothing provides -- so the checker itself is not untested even
// though the real reconciliation against engine artifacts only runs on CI.
//
// Run: node scripts/check-plugin-symbols.test.mjs

import assert from 'node:assert/strict';
import { reconcile } from './check-plugin-symbols.mjs';

/* --- minimal WASM encoder: functions only, all of type () -> () --- */

function uleb(n) {
	const out = [];
	do {
		let b = n & 0x7f;
		n >>>= 7;
		if (n) b |= 0x80;
		out.push(b);
	} while (n);
	return out;
}
function str(s) {
	const b = [...Buffer.from(s, 'utf8')];
	return [...uleb(b.length), ...b];
}
function vec(items) {
	return [...uleb(items.length), ...items.flat()];
}
function section(id, payload) {
	return [id, ...uleb(payload.length), ...payload];
}

// imports: [{ module, name }], exports: [name].  Every function is type 0,
// the empty signature () -> ().  Exported functions get an empty body.
function makeWasm({ imports = [], exports = [] } = {}) {
	const typeSec = section(1, vec([[0x60, 0x00, 0x00]]));
	const importSec = section(2, vec(imports.map((i) =>
		[...str(i.module), ...str(i.name), 0x00, 0x00])));
	const funcSec = section(3, vec(exports.map(() => [0x00])));
	const numImported = imports.length;
	const exportSec = section(7, vec(exports.map((name, i) =>
		[...str(name), 0x00, ...uleb(numImported + i)])));
	const body = [...uleb(2), 0x00, 0x0b]; /* 0 locals, then `end` */
	const codeSec = section(10, vec(exports.map(() => body)));
	return Uint8Array.from([
		0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
		...typeSec, ...importSec, ...funcSec, ...exportSec, ...codeSec,
	]);
}

/* --- fixtures mirroring the real engine's shape --- */

const main = makeWasm({
	imports: [{ module: 'env', name: 'emscripten_webgl_create_context' }],
	exports: ['subsystem_manager_register'],
});

const good = makeWasm({
	imports: [
		{ module: 'env', name: 'subsystem_manager_register' },      /* main export      */
		{ module: 'env', name: 'emscripten_webgl_create_context' }, /* main asmLibraryArg */
		{ module: 'wasi_snapshot_preview1', name: 'fd_write' },     /* runtime-provided */
	],
	exports: ['imgui_register_panel'],
});

const consumer = makeWasm({
	imports: [{ module: 'env', name: 'imgui_register_panel' }],     /* from good's exports */
	exports: [],
});

const bad = makeWasm({
	imports: [
		{ module: 'env', name: 'glClear' },
		{ module: 'env', name: 'glClearColor' },
	],
	exports: [],
});

/* --- assertions --- */

// 1) Everything resolves (incl. plugin-to-plugin and runtime imports).
assert.deepEqual(
	reconcile({ main, plugins: [
		{ name: 'good.wasm', bytes: good },
		{ name: 'consumer.wasm', bytes: consumer },
	]}),
	[],
	'expected every import to resolve');

// 2) Unprovided env functions are flagged -- exactly the GL bug we are trapping.
assert.deepEqual(
	reconcile({ main, plugins: [
		{ name: 'good.wasm', bytes: good },
		{ name: 'bad.wasm', bytes: bad },
	]}),
	[
		{ plugin: 'bad.wasm', symbol: 'glClear' },
		{ plugin: 'bad.wasm', symbol: 'glClearColor' },
	],
	'expected glClear/glClearColor to be flagged');

// 3) Runtime-module imports are never flagged, even with a bare main module.
assert.deepEqual(
	reconcile({ main: makeWasm({}), plugins: [
		{ name: 'w.wasm', bytes: makeWasm({ imports: [
			{ module: 'wasi_snapshot_preview1', name: 'fd_write' }] }) },
	]}),
	[],
	'expected wasi imports to be ignored');

console.log('check-plugin-symbols self-test: all assertions passed');
