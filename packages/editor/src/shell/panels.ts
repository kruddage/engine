// SPDX-License-Identifier: GPL-2.0-or-later
//
// The panel registry.
//
// A panel declares itself here and the shell places it. **Adding a panel must
// not mean editing the shell** — that is one of #954's acceptance criteria, and
// it is the criterion that decides whether #950, #951 and #952 are three small
// PRs or three PRs that each touch the frame everything else mounts into.
//
// The registry is a module-level list rather than a React context because the
// set of panels is fixed at build time — it is not state, nothing subscribes to
// it, and a context would invite something to mutate it at runtime.

import type { PanelSlot } from "./vocabulary.js";

/**
 * A panel's stable identity.
 *
 * These are the vocabulary's names, and they are what the persisted layout is
 * keyed by — so renaming one is a breaking change to every reader's stored
 * layout, and reconcile() below is what keeps that from being a crash.
 */
export type PanelId =
	| "outliner"
	| "inspector"
	| "assets"
	| "console"
	| "build"
	| "viewport";

export interface PanelDefinition {
	id: PanelId;
	/** The tab's label. */
	title: string;
	/** Where the shell puts it when there is no remembered layout. */
	home: PanelSlot;
	/**
	 * Whether the reader may hide it. The viewport cannot be hidden — a shell
	 * with no viewport is not an editor, it is a settings dialog.
	 */
	hideable?: boolean;
	/**
	 * Whether the reader may move it between slots. False for the deck; see the
	 * note on PanelSlot for why that is a correctness constraint rather than a
	 * layout preference.
	 */
	movable?: boolean;
	render: () => React.JSX.Element;
}

const registry = new Map<PanelId, PanelDefinition>();

/**
 * Register a panel. Later registrations of the same id replace earlier ones,
 * which is what lets a test swap a panel for a stub without the shell knowing.
 */
export function registerPanel(definition: PanelDefinition): void {
	registry.set(definition.id, {
		hideable: true,
		movable: true,
		...definition,
	});
}

/** Every registered panel, in registration order. */
export function panels(): PanelDefinition[] {
	return [...registry.values()];
}

export function panel(id: PanelId): PanelDefinition | undefined {
	return registry.get(id);
}

/** For tests that need a registry with only what they put in it. */
export function clearPanelsForTests(): void {
	registry.clear();
}
