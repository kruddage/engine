// SPDX-License-Identifier: GPL-2.0-or-later
//
// CI gate: fail on raw libc allocation calls outside the memory subsystem.
//
// Modules and plugins must allocate through mem_alloc / mem_alloc_zero /
// mem_free (native) or the memory_api vtable (side modules) -- never call
// libc malloc/calloc/realloc/free (and friends) directly. That keeps the
// engine allocator (and, on WASM, the mimalloc heap the memory module owns)
// as the single ABI seam plugins go through.
//
// Flags word-boundary CALLS -- an identifier immediately followed by "(" --
// to the banned names below, except when the identifier is reached through a
// vtable (preceded by "->" or "."), e.g. `mem->free(...)` / `api.free(...)`.
// Comments and string/char literals are stripped before matching so prose
// that merely mentions these names (e.g. "the free() hook") does not trip
// the check.
//
// Usage: node scripts/check-allocations.mjs [DIR]   (default: repo root)

import { readFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { join } from 'node:path';

const BANNED = [
	'malloc', 'calloc', 'realloc', 'free',
	'aligned_alloc', 'posix_memalign', 'strdup', 'strndup',
];
const CALL_PATTERN = new RegExp(`\\b(${BANNED.join('|')})\\s*\\(`, 'g');

// Exempt paths: the allocator implementation itself, tests (which may link
// libc / the memory module as they see fit), and vendored third-party trees.
const EXEMPT = [
	/(^|\/)modules\/memory\//,
	/_test\.(c|cpp)$/,
	/(^|\/)vendor\//,
];

export function isExempt(path) {
	return EXEMPT.some((re) => re.test(path));
}

// Blank out comments and string/char literal contents (preserving newlines,
// so reported line numbers stay accurate) so prose or literal text that
// happens to contain a banned name never trips the call-form regex above.
export function stripCommentsAndStrings(src) {
	let out = '';
	let state = 'code'; // code | line_comment | block_comment | string | char
	let i = 0;
	const n = src.length;

	while (i < n) {
		const c = src[i];
		const c2 = src.slice(i, i + 2);

		if (state === 'code') {
			if (c2 === '//') { state = 'line_comment'; out += '  '; i += 2; continue; }
			if (c2 === '/*') { state = 'block_comment'; out += '  '; i += 2; continue; }
			if (c === '"') { state = 'string'; out += ' '; i += 1; continue; }
			if (c === "'") { state = 'char'; out += ' '; i += 1; continue; }
			out += c;
			i += 1;
			continue;
		}
		if (state === 'line_comment') {
			if (c === '\n') { state = 'code'; out += '\n'; i += 1; continue; }
			out += ' ';
			i += 1;
			continue;
		}
		if (state === 'block_comment') {
			if (c2 === '*/') { state = 'code'; out += '  '; i += 2; continue; }
			out += c === '\n' ? '\n' : ' ';
			i += 1;
			continue;
		}
		// string / char literal
		if (c === '\\') { out += '  '; i += 2; continue; }
		if ((state === 'string' && c === '"') ||
		    (state === 'char' && c === "'")) {
			state = 'code';
			out += ' ';
			i += 1;
			continue;
		}
		out += c === '\n' ? '\n' : ' ';
		i += 1;
	}
	return out;
}

// Returns [{ path, line, name }] for every banned call in `src` that is not
// reached through a vtable pointer (`->name(` / `.name(`).
export function findViolations(path, src) {
	const stripped = stripCommentsAndStrings(src);
	const violations = [];
	let m;

	CALL_PATTERN.lastIndex = 0;
	while ((m = CALL_PATTERN.exec(stripped))) {
		let j = m.index - 1;

		while (j >= 0 && /\s/.test(stripped[j]))
			j--;
		const prev = j >= 0 ? stripped[j] : '';
		const prevIsArrow = j >= 1 && stripped.slice(j - 1, j + 1) === '->';

		if (prev === '.' || prevIsArrow)
			continue;

		const line = stripped.slice(0, m.index).split('\n').length;
		violations.push({ path, line, name: m[1] });
	}
	return violations;
}

function main() {
	const dir = process.argv[2] || '.';
	const files = execFileSync('git', ['-C', dir, 'ls-files',
		'*.c', '*.cpp', '*.h', '*.hpp'], { encoding: 'utf8' })
		.split('\n')
		.filter(Boolean)
		.filter((f) => !isExempt(f));

	const all = [];
	for (const f of files) {
		const src = readFileSync(join(dir, f), 'utf8');

		all.push(...findViolations(f, src));
	}

	if (all.length > 0) {
		console.error('FAIL: raw libc allocation calls outside the memory subsystem:\n');
		for (const v of all)
			console.error(`${v.path}:${v.line}: raw call to ${v.name}()`);
		console.error('\nAllocate through mem_alloc/mem_alloc_zero/mem_free (native) or the\n' +
			'memory_api vtable (plugins) instead. See plugins/include/memory_api.h.');
		process.exit(1);
	}

	console.log(`OK: no raw allocation calls outside the memory subsystem (${files.length} files scanned)`);
}

// Run the CLI only when invoked directly, not when imported by the self-test.
if (import.meta.url === `file://${process.argv[1]}`)
	main();
