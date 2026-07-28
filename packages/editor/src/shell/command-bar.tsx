// SPDX-License-Identifier: GPL-2.0-or-later
//
// The command bar: File, Edit, View, Help.
//
// Built on `@radix-ui/react-menubar` rather than hand-rolled. What that buys is
// the part of a menu bar that is invisible when it works and a bug report when
// it does not — roving focus across the bar, arrow keys within a menu, type-
// ahead, Escape, focus returning to the trigger it came from, `aria-expanded`
// and `aria-haspopup`, and a submenu that opens on the right edge of the screen
// by flipping instead of clipping. #944's rule is to take the package and write
// down what replacing it would cost; replacing this one costs a keyboard-
// accessible menu implementation, which is a month and a permanent liability.
//
// The menu *content* is entirely ours — see commands.ts. Radix contributes
// behaviour, not vocabulary.

import * as Menubar from "@radix-ui/react-menubar";

import {
	MENUS,
	chordFor,
	chordLabel,
	stripMnemonic,
	type Menu,
	type MenuItem,
} from "./commands.js";
import type { PanelDefinition, PanelId } from "./panels.js";
import type { LayoutState } from "./layout.js";
import { DOCKABLE_SLOTS, SLOT_LABEL, type PanelSlot } from "./vocabulary.js";

export interface CommandBarProps {
	panels: PanelDefinition[];
	layout: LayoutState;
	onAction: (id: string, label: string) => void;
	onTogglePanel: (id: PanelId) => void;
	onMovePanel: (id: PanelId, slot: PanelSlot) => void;
}

export function CommandBar({
	panels,
	layout,
	onAction,
	onTogglePanel,
	onMovePanel,
}: CommandBarProps): React.JSX.Element {
	return (
		<Menubar.Root className="command-bar" aria-label="Command bar">
			{MENUS.map((menu) => (
				<MenuTrigger
					key={menu.label}
					menu={menu}
					panels={panels}
					layout={layout}
					onAction={onAction}
					onTogglePanel={onTogglePanel}
					onMovePanel={onMovePanel}
				/>
			))}
		</Menubar.Root>
	);
}

function MenuTrigger({
	menu,
	panels,
	layout,
	onAction,
	onTogglePanel,
	onMovePanel,
}: CommandBarProps & { menu: Menu }): React.JSX.Element {
	return (
		<Menubar.Menu>
			<Menubar.Trigger className="command-bar__trigger">
				{stripMnemonic(menu.label)}
			</Menubar.Trigger>
			<Menubar.Portal>
				<Menubar.Content className="menu" align="start" sideOffset={2}>
					{menu.items.map((item, index) => (
						<Item
							/* Separators and expansions have no identity of their
							 * own, and the list is static — the index is the
							 * honest key here rather than a fabricated id. */
							key={`${menu.label}:${index}`}
							item={item}
							panels={panels}
							layout={layout}
							onAction={onAction}
							onTogglePanel={onTogglePanel}
							onMovePanel={onMovePanel}
						/>
					))}
				</Menubar.Content>
			</Menubar.Portal>
		</Menubar.Menu>
	);
}

function Item({
	item,
	panels,
	layout,
	onAction,
	onTogglePanel,
	onMovePanel,
}: CommandBarProps & { item: MenuItem }): React.JSX.Element | null {
	if (item.kind === "separator") {
		return <Menubar.Separator className="menu__separator" />;
	}

	if (item.kind === "panel-toggles") {
		return (
			<>
				{panels
					.filter((definition) => definition.hideable !== false)
					.map((definition) => (
						<Menubar.CheckboxItem
							key={definition.id}
							className="menu__item"
							checked={!layout.hidden.includes(definition.id)}
							onSelect={() => onTogglePanel(definition.id)}
						>
							<Menubar.ItemIndicator className="menu__check">
								✓
							</Menubar.ItemIndicator>
							{definition.title}
						</Menubar.CheckboxItem>
					))}
			</>
		);
	}

	if (item.kind === "panel-moves") {
		const movable = panels.filter((definition) => definition.movable !== false);
		if (movable.length === 0) return null;
		return (
			<Menubar.Sub>
				<Menubar.SubTrigger className="menu__item menu__item--sub">
					Move
				</Menubar.SubTrigger>
				<Menubar.Portal>
					<Menubar.SubContent className="menu">
						{movable.map((definition) => (
							<Menubar.Sub key={definition.id}>
								<Menubar.SubTrigger className="menu__item menu__item--sub">
									{definition.title}
								</Menubar.SubTrigger>
								<Menubar.Portal>
									<Menubar.SubContent className="menu">
										{DOCKABLE_SLOTS.map((slot) => (
											<Menubar.Item
												key={slot}
												className="menu__item"
												onSelect={() =>
													onMovePanel(definition.id, slot)
												}
											>
												{SLOT_LABEL[slot]}
											</Menubar.Item>
										))}
									</Menubar.SubContent>
								</Menubar.Portal>
							</Menubar.Sub>
						))}
					</Menubar.SubContent>
				</Menubar.Portal>
			</Menubar.Sub>
		);
	}

	const chord = chordLabel(chordFor(item.shortcut));
	return (
		<Menubar.Item
			className="menu__item"
			onSelect={() => onAction(item.id, item.label)}
		>
			<span className="menu__label">{stripMnemonic(item.label)}</span>
			{chord !== "" && <span className="menu__chord">{chord}</span>}
		</Menubar.Item>
	);
}
