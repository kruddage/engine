// SPDX-License-Identifier: GPL-2.0-or-later
//
// Plugin symbol-resolution check (load-order aware).
//
// The engine is an Emscripten MAIN_MODULE; plugins are built as SIDE_MODULEs and
// loaded with emscripten_dlopen. A side module's function imports are bound, once,
// at instantiation, against the global symbol table as it exists AT THAT MOMENT:
//
//   * the main module's WASM exports (its EMSCRIPTEN_KEEPALIVE C functions and the
//     libc/libc++ it statically links),
//   * the JS-library functions the main module pulled into asmLibraryArg -- i.e.
//     precisely the env function imports of the MAIN module, since a JS-library
//     function is included only when the main module's own WASM imports it, and
//   * the WASM exports of plugins that have ALREADY been loaded.
//
// If a plugin imports an `env` function that is not yet provided, Emscripten's
// dynamic linker does NOT error -- it substitutes a stub that THROWS on first
// call, surfacing in the browser as an uncaught JS runtime exception. Two ways
// this happens:
//
//   * UNRESOLVED -- nothing anywhere provides the symbol (e.g. a gl* function the
//     main module forgot to wire into asmLibraryArg; see modules/core/plugin_abi.c).
//   * ORDER -- the symbol IS provided by another plugin, but that plugin loads
//     LATER (e.g. kruddboard calls imgui_plugin's ImGui symbols). Plugins must be
//     loaded in dependency order; the loader serializes on plugin_list order, so
//     that list must be a valid topological order.
//
// This check walks the plugins in their declared load order (parsed from the
// plugins[] table in modules/core/engine.c) and fails fast listing every import
// that would become a throwing stub, classified as UNRESOLVED or ORDER.
//
// Pure Node -- no emcc, no WABT. Reads already-built .wasm via the WebAssembly
// reflection API.
//
// Usage: node scripts/check-plugin-symbols.mjs [BUILD_DIR]   (default: build)

import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const MAIN = 'index.wasm';

// Imports from these modules are satisfied by the Emscripten runtime itself,
// not by the main module or other plugins, so they never become stubs.
const RUNTIME_MODULES = new Set(['wasi_snapshot_preview1']);

// Symbols the dynamic linker / runtime always provides even though they appear
// in neither the main module's exports nor its asmLibraryArg. Keep this minimal:
// add an entry only once a real CI run proves it is a benign linker internal, so
// that genuine gaps are never masked.
const ALLOWLIST = new Set([]);

export function envFunctionImports(bytes) {
	const mod = new WebAssembly.Module(bytes);
	const out = new Set();
	for (const imp of WebAssembly.Module.imports(mod)) {
		if (imp.kind === 'function' && !RUNTIME_MODULES.has(imp.module))
			out.add(imp.name);
	}
	return out;
}

export function functionExports(bytes) {
	const mod = new WebAssembly.Module(bytes);
	const out = new Set();
	for (const exp of WebAssembly.Module.exports(mod)) {
		if (exp.kind === 'function')
			out.add(exp.name);
	}
	return out;
}

// Parse the ordered list of plugin .wasm names from the plugins[] table in
// engine.c. This is the order the loader loads them in, and therefore the order
// in which their exports become available to subsequent plugins.
export function loadOrder(engineSrc) {
	const m = engineSrc.match(/plugins\[\]\s*=\s*\{([\s\S]*?)\}/);
	if (!m)
		throw new Error('could not find plugins[] table in engine.c');
	return [...m[1].matchAll(/"([^"]+\.wasm)"/g)].map((x) => x[1]);
}

// Walk plugins in load order. Returns a sorted list of problems:
//   { plugin, symbol, kind: 'unresolved' }            -- nothing provides it, or
//   { plugin, symbol, kind: 'order', provider }       -- provided, but too late.
export function reconcile({ main, plugins }) {
	const provided = new Set([
		...functionExports(main),
		...envFunctionImports(main),
	]);
	const exportsOf = plugins.map((p) => ({
		name: p.name,
		exports: functionExports(p.bytes),
	}));

	const problems = [];
	for (let i = 0; i < plugins.length; i++) {
		for (const sym of envFunctionImports(plugins[i].bytes)) {
			if (provided.has(sym) || ALLOWLIST.has(sym))
				continue;
			const later = exportsOf
				.slice(i + 1)
				.find((e) => e.exports.has(sym));
			problems.push(later
				? { plugin: plugins[i].name, symbol: sym,
				    kind: 'order', provider: later.name }
				: { plugin: plugins[i].name, symbol: sym,
				    kind: 'unresolved' });
		}
		for (const s of exportsOf[i].exports)
			provided.add(s);
	}

	problems.sort((a, b) =>
		a.plugin === b.plugin
			? a.symbol.localeCompare(b.symbol)
			: a.plugin.localeCompare(b.plugin));
	return problems;
}

function runCli() {
	const buildDir = process.argv[2] || 'build';

	const engineSrc = readFileSync(
		new URL('../modules/core/engine.c', import.meta.url), 'utf8');
	const order = loadOrder(engineSrc);

	const mainPath = join(buildDir, MAIN);
	if (!existsSync(mainPath)) {
		console.error(`error: ${mainPath} not found -- build first`);
		process.exit(2);
	}
	const main = readFileSync(mainPath);

	const plugins = [];
	for (const name of order) {
		const p = join(buildDir, name);
		if (!existsSync(p)) {
			console.error(`error: ${name} is listed in engine.c ` +
				`plugins[] but ${p} is missing`);
			process.exit(2);
		}
		plugins.push({ name, bytes: readFileSync(p) });
	}

	const problems = reconcile({ main, plugins });

	if (problems.length === 0) {
		console.log(
			`OK: all ${plugins.length} plugin(s) resolve every env ` +
			`function import in load order ` +
			`[${order.join(', ')}]`);
		return;
	}

	console.error(
		'FAIL: plugin function imports that become stubs which throw at ' +
		'runtime:\n');
	let last = '';
	for (const p of problems) {
		if (p.plugin !== last) {
			console.error(`  ${p.plugin}:`);
			last = p.plugin;
		}
		if (p.kind === 'order')
			console.error(`      ${p.symbol}  ` +
				`(ORDER: provided by ${p.provider}, which loads later)`);
		else
			console.error(`      ${p.symbol}  (UNRESOLVED: no provider)`);
	}

	const orders = problems.filter((p) => p.kind === 'order').length;
	const unres = problems.length - orders;
	console.error(
		`\n${unres} unresolved (wire into the main module -- see ` +
		`modules/core/plugin_abi.c), ${orders} ordering ` +
		`violation(s) (fix plugins[] order in engine.c).`);
	process.exit(1);
}

// Run the CLI only when invoked directly, not when imported by the self-test.
if (import.meta.url === `file://${process.argv[1]}`)
	runCli();
