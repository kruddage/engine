// SPDX-License-Identifier: GPL-2.0-or-later
//
// Self-test for check-plugin-symbols.mjs. Builds tiny synthetic WASM modules in
// memory (no emcc required) and asserts the load-order-aware reconciliation
// flags exactly the imports that nothing provides in time -- so the checker is
// covered without a toolchain. Only the final reconciliation against the real
// engine artifacts needs a build, which runs on CI.
//
// Run: node scripts/check-plugin-symbols.test.mjs

import assert from 'node:assert/strict';
import { reconcile, loadOrder } from './check-plugin-symbols.mjs';

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

// imports: [{ module, name }] function imports; globalImports: [{ module, name }]
// imported i32 globals (used to model GOT.func.<name> address-of entries).
// exports: [name] exported functions with empty bodies (all type () -> ()).
function makeWasm({ imports = [], globalImports = [], exports = [] } = {}) {
	const typeSec = section(1, vec([[0x60, 0x00, 0x00]]));
	const importSec = section(2, vec([
		...imports.map((i) =>
			[...str(i.module), ...str(i.name), 0x00, 0x00]),
		...globalImports.map((g) =>
			[...str(g.module), ...str(g.name), 0x03, 0x7f, 0x00]),
	]));
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
const env = (name) => ({ module: 'env', name });

/* --- fixtures mirroring the real engine's shape --- */

// Main exports an engine symbol and pulls one JS-library fn into asmLibraryArg.
const main = makeWasm({
	imports: [env('emscripten_webgl_create_context')],
	exports: ['subsystem_manager_register'],
});

// renderer: resolves entirely against the main module.
const renderer = makeWasm({
	imports: [env('subsystem_manager_register'),
		  env('emscripten_webgl_create_context')],
	exports: [],
});
// imgui: exports the ImGui symbols.
const imgui = makeWasm({
	imports: [env('subsystem_manager_register')],
	exports: ['_ZN5ImGui5BeginEPKcPbi', '_ZN5ImGui3EndEv'],
});
// kruddboard: imports ImGui symbols + a runtime import that must be ignored.
const kruddboard = makeWasm({
	imports: [
		env('_ZN5ImGui5BeginEPKcPbi'),
		env('_ZN5ImGui3EndEv'),
		{ module: 'wasi_snapshot_preview1', name: 'fd_write' },
	],
	exports: [],
});
const p = (name, bytes) => ({ name, bytes });

/* --- assertions --- */

// 1) Correct load order: imgui before kruddboard -> everything resolves.
assert.deepEqual(
	reconcile({ main, plugins: [
		p('renderer_webgl.wasm', renderer),
		p('imgui_plugin.wasm', imgui),
		p('kruddboard.wasm', kruddboard),
	]}),
	[],
	'expected every import to resolve in dependency order');

// 2) Wrong order: kruddboard before imgui -> ORDER violation naming the provider.
assert.deepEqual(
	reconcile({ main, plugins: [
		p('kruddboard.wasm', kruddboard),
		p('imgui_plugin.wasm', imgui),
	]}),
	[
		{ plugin: 'kruddboard.wasm', symbol: '_ZN5ImGui3EndEv',
		  kind: 'order', provider: 'imgui_plugin.wasm' },
		{ plugin: 'kruddboard.wasm', symbol: '_ZN5ImGui5BeginEPKcPbi',
		  kind: 'order', provider: 'imgui_plugin.wasm' },
	],
	'expected ordering violations for kruddboard before imgui');

// 3) Nothing provides it -> UNRESOLVED (the original GL bug class).
assert.deepEqual(
	reconcile({ main, plugins: [
		p('renderer_webgl.wasm', makeWasm({ imports: [env('glClear')] })),
	]}),
	[{ plugin: 'renderer_webgl.wasm', symbol: 'glClear', kind: 'unresolved' }],
	'expected glClear to be unresolved');

// 4) Weak C++ symbols a module both imports and exports self-resolve and must
//    not be flagged (regression: imgui_plugin's 92 ImVector<...> weak symbols).
assert.deepEqual(
	reconcile({ main, plugins: [
		p('imgui_plugin.wasm', makeWasm({
			imports: [env('_ZN8ImVectorIiE9push_backERKi')],
			exports: ['_ZN8ImVectorIiE9push_backERKi'],
		})),
	]}),
	[],
	'expected a module’s own weak exports to satisfy its own imports');

// 5) A symbol the main module only address-takes appears as a GOT.func import
//    (not env). At -O2 that's all there is, yet it IS callable by side modules,
//    so it must count as provided (regression: the Release build's gl* symbols).
assert.deepEqual(
	reconcile({
		main: makeWasm({ globalImports: [{ module: 'GOT.func', name: 'glClear' }] }),
		plugins: [p('renderer_webgl.wasm', makeWasm({ imports: [env('glClear')] }))],
	}),
	[],
	'expected a GOT.func address-of in main to satisfy a plugin’s env import');

// 6) loadOrder parses the plugins[] table from engine.c verbatim, in order.
const engineSrc = `
static const char * const plugins[] = {
	"hello_plugin.wasm",
	"asset_plugin.wasm",
	"renderer_webgl.wasm",
	"imgui_plugin.wasm",
	"kruddboard.wasm",
	NULL,
};`;
assert.deepEqual(
	loadOrder(engineSrc),
	['hello_plugin.wasm', 'asset_plugin.wasm', 'renderer_webgl.wasm',
	 'imgui_plugin.wasm', 'kruddboard.wasm'],
	'expected loadOrder to parse plugins[] in declared order');

console.log('check-plugin-symbols self-test: all assertions passed');
