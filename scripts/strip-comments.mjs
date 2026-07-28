// SPDX-License-Identifier: GPL-2.0-or-later
//
// Removes comments from JS source, leaving string and template literals intact.
//
// check-barriers.mjs needs this because the names it forbids are exactly the
// names the surrounding prose has to discuss — the first thing the check did on
// being written was fail its own author, over a comment explaining that the
// file deliberately did not touch krudd.sh. A boundary check that cannot tell
// code from commentary gets switched off within a week.
//
// Comments become spaces rather than vanishing, so byte offsets and line
// numbers survive for anything that wants to report a position.

/* Whether a `/` here starts a regex literal rather than a division. The usual
 * heuristic: look back at the last meaningful character. After a value (an
 * identifier, a literal, a closing bracket) `/` divides; after an operator or
 * an opening bracket it starts a pattern. `)` is the ambiguous case — `if (x)
 * /re/.test(s)` is legal — but that shape does not occur in this workspace, and
 * guessing division there is the safe way to be wrong: a mis-parsed regex only
 * risks a spurious match inside it, never a missed violation in real code. */
function regexAllowedAfter(previous) {
	if (previous === "") return true;
	return "(,=:[!&|?{};+-*%~^<>".includes(previous);
}

export function stripComments(source) {
	let out = "";
	let i = 0;
	let previous = "";

	const blank = (text) => text.replace(/[^\n]/g, " ");

	while (i < source.length) {
		const ch = source[i];
		const next = source[i + 1];

		if (ch === "/" && next === "/") {
			const end = source.indexOf("\n", i);
			const stop = end === -1 ? source.length : end;
			out += blank(source.slice(i, stop));
			i = stop;
			continue;
		}

		if (ch === "/" && next === "*") {
			const end = source.indexOf("*/", i + 2);
			const stop = end === -1 ? source.length : end + 2;
			out += blank(source.slice(i, stop));
			i = stop;
			continue;
		}

		if (ch === '"' || ch === "'" || ch === "`") {
			let j = i + 1;
			while (j < source.length) {
				if (source[j] === "\\") {
					j += 2;
					continue;
				}
				if (source[j] === ch) break;
				j++;
			}
			out += source.slice(i, Math.min(j + 1, source.length));
			i = j + 1;
			previous = ch;
			continue;
		}

		if (ch === "/" && regexAllowedAfter(previous)) {
			let j = i + 1;
			let inClass = false;
			while (j < source.length) {
				if (source[j] === "\\") {
					j += 2;
					continue;
				}
				if (source[j] === "[") inClass = true;
				else if (source[j] === "]") inClass = false;
				else if (source[j] === "/" && !inClass) break;
				else if (source[j] === "\n") break;
				j++;
			}
			out += source.slice(i, Math.min(j + 1, source.length));
			i = j + 1;
			previous = "/";
			continue;
		}

		out += ch;
		if (!/\s/.test(ch)) previous = ch;
		i++;
	}

	return out;
}
