// SPDX-License-Identifier: GPL-2.0-or-later
//
// Two rules about the stylesheet, asserted rather than written down and hoped
// for. Both are #954 acceptance criteria and both are the kind that a later PR
// breaks by accident in one line.
//
// Reading the file as text is the only honest way to check either: jsdom does
// not implement container queries, so a rendered assertion would pass on a
// stylesheet that had been quietly rewritten to media queries.
//
// The text comes through Vite's `?raw` rather than node:fs, because this half
// of the package deliberately has no Node types — `src/` and `test/` have no
// business reaching a filesystem, and tsconfig.json leaving `node:fs`
// undefined is the guard that says so.

import css from "../src/styles.css?raw";

import { describe, expect, it } from "vitest";

const CSS = css;

/* Comments carry both forbidden shapes as prose — stripping them first is what
 * keeps this suite from failing on the note that explains the rule. */
const RULES = CSS.replace(/\/\*[\s\S]*?\*\//g, "");

describe("container queries, not width media queries", () => {
	it("adapts on the container's width", () => {
		expect(RULES).toMatch(/@container\s/);
		expect(RULES).toMatch(/container-type:\s*inline-size/);
	});

	it("has no width or height media query anywhere", () => {
		/*
		 * A panel that answers to the window rather than to its own frame lays
		 * out wrongly the moment a dock is dragged narrow while the window
		 * stays wide — which is the normal state of an editor.
		 */
		const offenders = [...RULES.matchAll(/@media[^{]*/g)]
			.map((match) => match[0])
			.filter((query) => /\b(min|max)-(width|height)\b|\bwidth\s*[<>]/.test(query));

		expect(offenders).toEqual([]);
	});

	it("still allows the media queries that are not about size", () => {
		/* prefers-color-scheme and prefers-reduced-motion are questions about
		 * the reader, not about the viewport, and they belong in @media. */
		expect(RULES).toMatch(/@media\s*\(prefers-color-scheme/);
	});
});

describe("the reader's font size", () => {
	it("never fixes a font size in pixels", () => {
		const offenders = [...RULES.matchAll(/font-size:\s*[^;]+;/g)]
			.map((match) => match[0])
			.filter((declaration) => /\d\s*px/.test(declaration));

		expect(offenders).toEqual([]);
	});

	it("never overrides the root font size", () => {
		/*
		 * `html { font-size: 16px }` is the single declaration that undoes the
		 * size someone set because they cannot read the default. It is worth a
		 * test of its own rather than being covered by the rule above.
		 */
		expect(RULES).not.toMatch(/(?:^|[},])\s*(?:html|:root)[^{]*\{[^}]*font-size/);
	});
});
