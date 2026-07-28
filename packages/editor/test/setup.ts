// SPDX-License-Identifier: GPL-2.0-or-later
//
// Component-suite setup.
//
// Two jobs: isolation between tests, and the handful of browser APIs jsdom does
// not implement that the shell's dependencies expect to exist.
//
// The second list is worth being suspicious of. Every stub here is a place
// where jsdom is not a browser, and #944's Q6 rule is that a test goes to
// Playwright when it would lie in Vitest. These do not make anything lie —
// nothing in the component suite asserts on a size, a pointer capture or a
// media query, and each stub exists so a *dependency* can mount rather than so
// a test can pass. Resize behaviour, pointer arbitration and real layout are
// Playwright's, and they stay there.

import { afterEach, vi } from "vitest";
import { cleanup } from "@testing-library/react";

import { resetBootStateForTests } from "../src/engine/boot.js";
import { clearPanelsForTests } from "../src/shell/panels.js";

/*
 * react-resizable-panels observes its group to solve constraints, and Radix
 * observes popover content to position it. Neither needs to report anything
 * here — a no-op observer lets both mount, and jsdom reports every element as
 * zero-sized regardless.
 */
if (typeof globalThis.ResizeObserver !== "function") {
	globalThis.ResizeObserver = class {
		observe(): void {}
		unobserve(): void {}
		disconnect(): void {}
	} as unknown as typeof ResizeObserver;
}

/* isCoarsePointer() asks matchMedia at module scope. */
if (typeof window !== "undefined" && typeof window.matchMedia !== "function") {
	window.matchMedia = ((query: string) => ({
		matches: false,
		media: query,
		onchange: null,
		addListener: () => {},
		removeListener: () => {},
		addEventListener: () => {},
		removeEventListener: () => {},
		dispatchEvent: () => false,
	})) as unknown as typeof window.matchMedia;
}

/* Radix's dismissable layers use pointer capture; jsdom has the events but not
 * the capture API. */
if (typeof Element !== "undefined") {
	Element.prototype.hasPointerCapture ??= () => false;
	Element.prototype.setPointerCapture ??= () => {};
	Element.prototype.releasePointerCapture ??= () => {};
	Element.prototype.scrollIntoView ??= () => {};
}

afterEach(() => {
	cleanup();
	/* boot.ts guards against double-booting with module-scoped state — correct
	 * for a page, wrong for a suite where every test is a fresh page. */
	resetBootStateForTests();
	/* Same shape of problem: the panel registry is module scope, and a test
	 * that registers its own panels must not leak them into the next. */
	clearPanelsForTests();
	vi.useRealTimers();
});
