// SPDX-License-Identifier: GPL-2.0-or-later
//
// What a panel is allowed to know about the engine.
//
// A panel registers itself with a zero-argument `render()` — that is what lets
// adding a panel not mean editing the shell — so anything it needs has to reach
// it some other way. This context is that way, and it is deliberately small:
// the engine's identity, how far it got, and its output. Nothing here lets a
// panel mutate anything, because nothing at this stage can (#945 is the
// boundary; this shell is read-only by construction).
//
// The canvas ref is in here rather than passed down because exactly one panel
// takes it — the viewport — and threading a ref through the dock, the tab group
// and the panel registry to reach it would put the canvas in the vocabulary of
// three components that have no business knowing what one is.

import { createContext, useContext } from "react";

import type { EngineInfo, EngineStatus } from "./types.js";

export interface EngineContextValue {
	engine: EngineInfo;
	status: EngineStatus;
	log: readonly string[];
	canvasRef: React.RefObject<HTMLCanvasElement | null>;
}

const EngineContext = createContext<EngineContextValue | null>(null);

export const EngineProvider = EngineContext.Provider;

/**
 * The engine, from inside a panel.
 *
 * Throws rather than returning null: a panel rendered outside the shell is a
 * wiring mistake, and every call site would otherwise carry a null check for a
 * state that cannot legitimately occur.
 */
export function useEngineContext(): EngineContextValue {
	const value = useContext(EngineContext);
	if (value === null) {
		throw new Error("a panel was rendered outside the shell's EngineProvider");
	}
	return value;
}
