// SPDX-License-Identifier: GPL-2.0-or-later
//
// Self-test for check-allocations.mjs. Exercises the comment/string-literal
// stripper and the vtable-call exemption against small synthetic C sources so
// the checker's matching logic is covered without needing the real tree.
//
// Run: node scripts/check-allocations.test.mjs

import assert from 'node:assert/strict';
import { findViolations, isExempt } from './check-allocations.mjs';

function names(violations) {
	return violations.map((v) => `${v.line}:${v.name}`);
}

// --- direct libc calls are flagged -----------------------------------------

{
	const src = [
		'void *f(void)',
		'{',
		'	void *p = malloc(16);',
		'	free(p);',
		'	return calloc(1, 16);',
		'}',
	].join('\n');
	const v = findViolations('f.c', src);

	assert.deepEqual(names(v), ['3:malloc', '4:free', '5:calloc']);
}

// --- vtable calls (-> and .) are exempt -------------------------------------

{
	const src = [
		'void f(struct memory_api *mem, struct memory_api api)',
		'{',
		'	void *p = mem->alloc(16);',
		'	mem->free(p);',
		'	api.free(p);',
		'}',
	].join('\n');

	assert.deepEqual(findViolations('f.c', src), []);
}

// --- identifiers merely containing a banned name are not flagged -----------

{
	const src = 'static void *webgl_gpu_malloc(size_t n) { return xmalloc(n); }';

	assert.deepEqual(findViolations('f.c', src), []);
}

// --- comments mentioning a banned name are not flagged ----------------------

{
	const src = [
		'/*',
		' * releases it through the optional free() hook when dropped',
		' */',
		'// calls free() too',
		'void f(void) {}',
	].join('\n');

	assert.deepEqual(findViolations('f.c', src), []);
}

// --- a banned name inside a string/char literal is not flagged -------------

{
	const src = 'const char *s = "call malloc() here"; char c = \'x\';';

	assert.deepEqual(findViolations('f.c', src), []);
}

// --- line numbers stay accurate across a stripped block comment ------------

{
	const src = [
		'/* multi',
		' * line',
		' * comment mentioning free() */',
		'void f(void)',
		'{',
		'	free(0);',
		'}',
	].join('\n');

	assert.deepEqual(names(findViolations('f.c', src)), ['6:free']);
}

// --- exempt-path matcher -----------------------------------------------------

assert.equal(isExempt('modules/memory/memory.c'), true);
assert.equal(isExempt('plugins/entity_plugin/entity_test.c'), true);
assert.equal(isExempt('plugins/imgui_plugin/imgui_plugin_test.cpp'), true);
assert.equal(isExempt('vendor/mimalloc-stub/include/mimalloc.h'), true);
assert.equal(isExempt('plugins/frame_graph/fg.c'), false);

console.log('check-allocations self-test: all assertions passed');
