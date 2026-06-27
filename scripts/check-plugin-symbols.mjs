// SPDX-License-Identifier: GPL-2.0-or-later
//
// Plugin symbol-resolution check.
//
// The engine is an Emscripten MAIN_MODULE; plugins are built as SIDE_MODULEs.
// A side module's function imports are resolved at dlopen time against the
// global symbol table, which is exactly:
//
//   * the main module's WASM exports (its EMSCRIPTEN_KEEPALIVE C functions),
//   * the JS-library functions the main module pulled into asmLibraryArg --
//     i.e. precisely the env function imports of the MAIN module, since a
//     JS-library function is included only when the main module's own WASM
//     imports it, and
//   * the WASM exports of every other loaded side module.
//
// If a plugin imports an `env` function that none of those provide, Emscripten's
// dynamic linker does NOT error -- it substitutes a stub that THROWS the first
// time the function is invoked. In the browser that surfaces as an uncaught JS
// runtime exception (e.g. the renderer calling glClear on the first frame), and
// it is completely invisible to a compile/link-only CI build.
//
// This check reconciles every plugin's env function imports against what the
// main module and the other plugins provide, and fails fast listing any symbol
// that would become a throwing stub. It is the general guard for the whole class
// of "a side module needs a symbol nobody wired into the main module" bugs that
// modules/core/plugin_abi.c exists to fix one at a time.
//
// Pure Node -- no emcc, no WABT. It reads already-built .wasm files with the
// standard WebAssembly reflection API.
//
// Usage: node scripts/check-plugin-symbols.mjs [BUILD_DIR]   (default: build)
//        The main module is BUILD_DIR/index.wasm; every other *.wasm is a plugin.

import { readFileSync, readdirSync } from 'node:fs';
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

// Returns a sorted list of { plugin, symbol } that would become throwing stubs
// at runtime because nothing in the loaded set provides them.
export function reconcile({ main, plugins }) {
	const provided = new Set([
		...functionExports(main),
		...envFunctionImports(main),
	]);
	for (const p of plugins)
		for (const sym of functionExports(p.bytes))
			provided.add(sym);

	const missing = [];
	for (const p of plugins) {
		for (const sym of envFunctionImports(p.bytes)) {
			if (!provided.has(sym) && !ALLOWLIST.has(sym))
				missing.push({ plugin: p.name, symbol: sym });
		}
	}
	missing.sort((a, b) =>
		a.plugin === b.plugin
			? a.symbol.localeCompare(b.symbol)
			: a.plugin.localeCompare(b.plugin));
	return missing;
}

function runCli() {
	const buildDir = process.argv[2] || 'build';
	const wasm = readdirSync(buildDir).filter((f) => f.endsWith('.wasm'));

	if (!wasm.includes(MAIN)) {
		console.error(`error: ${join(buildDir, MAIN)} not found -- build first`);
		process.exit(2);
	}
	const pluginFiles = wasm.filter((f) => f !== MAIN).sort();
	if (pluginFiles.length === 0) {
		console.error(`error: no plugin .wasm found in ${buildDir}`);
		process.exit(2);
	}

	const mainBytes = readFileSync(join(buildDir, MAIN));
	const plugins = pluginFiles.map((f) => ({
		name: f,
		bytes: readFileSync(join(buildDir, f)),
	}));

	const missing = reconcile({ main: mainBytes, plugins });

	if (missing.length === 0) {
		console.log(
			`OK: all ${pluginFiles.length} plugin(s) resolve every env ` +
			`function import against ${MAIN} and each other`);
		return;
	}

	console.error(
		'FAIL: plugin function imports with no provider -- each becomes a ' +
		'stub that throws at runtime:\n');
	let last = '';
	for (const { plugin, symbol } of missing) {
		if (plugin !== last) {
			console.error(`  ${plugin}:`);
			last = plugin;
		}
		console.error(`      ${symbol}`);
	}
	console.error(
		`\n${missing.length} unresolved import(s). Wire each into the main ` +
		`module so it lands in asmLibraryArg (see modules/core/plugin_abi.c).`);
	process.exit(1);
}

// Run the CLI only when invoked directly, not when imported by the self-test.
if (import.meta.url === `file://${process.argv[1]}`)
	runCli();
