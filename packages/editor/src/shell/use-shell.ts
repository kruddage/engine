// SPDX-License-Identifier: GPL-2.0-or-later
//
// The shell's own state: the layout, the transient hint, and the one place a
// menu item, a rail button or a keyboard chord all end up.
//
// Kept out of shell.tsx so the behaviour is reachable without rendering a
// dock splitter, and so the frame stays a description of the layout rather
// than a description of the layout plus everything it can do.

import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import type { CommandContext, DocumentActions } from "./commands.js";
import { runAction } from "./commands.js";
import type { PanelDefinition, PanelId } from "./panels.js";
import {
	browserStore,
	clearLayout,
	defaultLayout,
	loadLayout,
	saveLayout,
	type GroupLayout,
	type LayoutState,
	type LayoutStore,
	movePanel as move,
	setSizes as resize,
	activatePanel as raise,
	togglePanel as toggle,
} from "./layout.js";
import type { PanelSlot } from "./vocabulary.js";

/** How long a hint stays up. The generated shell uses the same 2.6 seconds. */
export const HINT_MS = 2600;

/**
 * The transient status-strip message, on its own.
 *
 * Extracted from useShell because two things need it and one of them is built
 * before the other: the shell routes actions to the document's verbs, and those
 * verbs report through the hint. Owning it here means App can hold the hint,
 * hand it to both, and get one message queue — rather than calling useShell
 * twice and forking the layout state to reach it.
 */
export interface Hint {
	text: string | null;
	show: (text: string) => void;
}

export function useHint(): Hint {
	const [text, setText] = useState<string | null>(null);
	const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

	useEffect(
		() => () => {
			if (timer.current !== null) clearTimeout(timer.current);
		},
		[]
	);

	const show = useCallback((next: string) => {
		setText(next);
		if (timer.current !== null) clearTimeout(timer.current);
		timer.current = setTimeout(() => setText(null), HINT_MS);
	}, []);

	return useMemo(() => ({ text, show }), [text, show]);
}

export interface Shell {
	layout: LayoutState;
	/** The transient status-strip message, or null. */
	hint: string | null;
	hintNow: (text: string) => void;
	togglePanel: (id: PanelId) => void;
	movePanel: (id: PanelId, slot: PanelSlot) => void;
	activatePanel: (slot: PanelSlot, id: PanelId) => void;
	setSizes: (group: string, sizes: GroupLayout) => void;
	resetLayout: () => void;
	/** Run a menu action by id. Unwired ids fall through to the hint. */
	run: (id: string, label: string) => void;
}

export function useShell(
	definitions: PanelDefinition[],
	store: LayoutStore = browserStore(),
	/*
	 * Undefined until the engine has booted, and on a page whose engine was
	 * never built. Passed in rather than reached for through a context because
	 * the shell is a frame: it routes actions and does not decide what they
	 * mean, and a hook call here would make it a consumer of the document.
	 */
	document?: DocumentActions | undefined,
	/*
	 * Supplied when something outside the shell also reports through it — see
	 * useHint. Its own is the default, so a test that only cares about the
	 * layout still calls useShell(definitions) and gets a working hint.
	 */
	hint_?: Hint | undefined
): Shell {
	/*
	 * The store is read once, lazily, at first render. Reading it in an effect
	 * would mean one frame of default layout before the reader's own arrives,
	 * which is a visible flash of docks jumping into place.
	 */
	const [layout, setLayout] = useState<LayoutState>(() =>
		loadLayout(store, definitions)
	);
	const ownHint = useHint();
	const { text: hint, show: hintNow } = hint_ ?? ownHint;

	/* Every change is persisted, including the ones a reader made by dragging a
	 * splitter — which is the one they would most notice losing. */
	useEffect(() => {
		saveLayout(store, layout);
	}, [store, layout]);

	const resetLayout = useCallback(() => {
		/*
		 * Cleared as well as replaced. Leaving the old value behind would mean
		 * a reset that a reload undoes, which is the only way a reset can be
		 * worse than doing nothing.
		 */
		clearLayout(store);
		setLayout(defaultLayout(definitions));
	}, [store, definitions]);

	const shell = useMemo<Shell>(() => {
		const context: CommandContext = {
			resetLayout,
			togglePanel: (id) => setLayout((prior) => toggle(prior, id)),
			movePanel: (id, slot) => setLayout((prior) => move(prior, id, slot)),
			hint: hintNow,
			document,
		};
		return {
			layout,
			hint,
			hintNow,
			togglePanel: context.togglePanel,
			movePanel: context.movePanel,
			activatePanel: (slot, id) =>
				setLayout((prior) => raise(prior, slot, id)),
			setSizes: (group, sizes) =>
				setLayout((prior) => resize(prior, group, sizes)),
			resetLayout,
			run: (id, label) => runAction(id, label, context),
		};
	}, [layout, hint, hintNow, resetLayout, document]);

	return shell;
}
