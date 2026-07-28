// SPDX-License-Identifier: GPL-2.0-or-later
//
// A dock: one slot's tab group, and the panel it has raised.
//
// Tabs come from `@radix-ui/react-tabs` — arrow-key navigation between tabs,
// the `tablist`/`tab`/`tabpanel` roles wired to each other, and focus following
// selection. All of it is what a hand-rolled tab strip omits and a screen
// reader then cannot use.
//
// ## The container query lives here
//
// Every dock is a query container (`container-type: inline-size`), so a panel
// inside it reflows on **the dock's** width rather than the window's. That is
// what makes a dock dragged narrow lay out correctly while the window stays
// wide, and it is the reason container queries go in even though #944 rules the
// phone out of scope: they cost days now and a rewrite of every panel later.

import * as Tabs from "@radix-ui/react-tabs";

import type { PanelDefinition, PanelId } from "./panels.js";
import type { PanelSlot } from "./vocabulary.js";

export interface DockProps {
	slot: PanelSlot;
	panels: PanelDefinition[];
	active: PanelId;
	onActivate: (slot: PanelSlot, id: PanelId) => void;
}

export function Dock({
	slot,
	panels,
	active,
	onActivate,
}: DockProps): React.JSX.Element {
	return (
		<Tabs.Root
			className="dock"
			data-slot={slot}
			data-testid={`dock-${slot}`}
			value={active}
			onValueChange={(next) => onActivate(slot, next as PanelId)}
			orientation="horizontal"
		>
			<Tabs.List className="dock__tabs" aria-label={`${slot} dock`}>
				{panels.map((definition) => (
					<Tabs.Trigger
						key={definition.id}
						className="dock__tab"
						value={definition.id}
						data-testid={`tab-${definition.id}`}
					>
						{definition.title}
					</Tabs.Trigger>
				))}
			</Tabs.List>

			{panels.map((definition) => (
				<Tabs.Content
					key={definition.id}
					className="dock__body"
					value={definition.id}
					/*
					 * Unmounted when not raised, which is Radix's default and is
					 * the right one here: a panel that is not on screen should
					 * not be querying the engine or holding a subscription open.
					 * When a panel needs to survive being tabbed away from —
					 * the console's scrollback is the obvious one — that is a
					 * decision for the panel to make by lifting its state, not
					 * for the dock to make for all of them.
					 */
					tabIndex={-1}
				>
					{definition.render()}
				</Tabs.Content>
			))}
		</Tabs.Root>
	);
}
