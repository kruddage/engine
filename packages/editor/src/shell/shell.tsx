// SPDX-License-Identifier: GPL-2.0-or-later
//
// The shell: the frame every panel mounts into.
//
// Read the vocabulary first (vocabulary.ts) — the names below are that list and
// nothing else. Top to bottom: command bar, toolbar, then the body (rail, then
// the docks around the viewport deck), then the status strip.
//
// ## Mounted once, not duplicated
//
// A panel is rendered from the registry, in exactly one slot, and there is no
// path in this file that constructs a second instance of one. That is cheap
// here and a rewrite of every panel later — #886's mockup grew three Inspectors
// that drifted apart, and that is the outcome this constraint exists to make
// impossible rather than merely discouraged.
//
// ## Resizing
//
// `react-resizable-panels` owns the splitters. What it buys is keyboard-
// resizable separators with the right ARIA, pointer capture that survives
// leaving the window, collapse and expand, and constraint solving across nested
// groups. Replacing it is a week of pointer maths and a permanent source of
// off-by-a-pixel bugs, for a control nobody will ever compliment.
//
// Layouts are persisted through the shell's own state rather than the library's
// storage hook, so there is one stored layout with one schema and one reset —
// two persistence mechanisms is how a reset stops resetting everything.

import { Group, Panel, Separator, type Layout } from "react-resizable-panels";

import { CommandBar } from "./command-bar.js";
import { Dock } from "./dock.js";
import { Rail } from "./rail.js";
import { StatusStrip } from "./status-strip.js";
import { Toolbar } from "./toolbar.js";
import { activeIn, visible } from "./layout.js";
import { panel, type PanelDefinition, type PanelId } from "./panels.js";
import { useShortcuts } from "./use-shortcuts.js";
import type { Shell as ShellState } from "./use-shell.js";
import type { PanelSlot } from "./vocabulary.js";
import type { FrameStats } from "../engine/frame-stats.js";
import type { EngineInfo, EngineStatus } from "../engine/types.js";

/* Starting sizes. Only used until the reader drags something. */
const DEFAULT_LEFT = "18%";
const DEFAULT_RIGHT = "22%";
const DEFAULT_BOTTOM = "28%";

export interface ShellProps {
	shell: ShellState;
	panels: PanelDefinition[];
	engine: EngineInfo;
	status: EngineStatus;
	stats: FrameStats;
}

export function Shell({
	shell,
	panels,
	engine,
	status,
	stats,
}: ShellProps): React.JSX.Element {
	useShortcuts(shell.run);

	/*
	 * onLayoutChanged, not onLayoutChange, and only for real interactions.
	 * The former fires once when the pointer is released; the latter fires on
	 * every pointer move, which would mean a localStorage write per frame of a
	 * drag. The isUserInteraction guard drops the mount-time and
	 * constraint-recompute calls, so remembering a size means the reader chose
	 * it rather than the library recomputing it.
	 */
	const remember =
		(group: string) =>
		(layout: Layout, meta: { isUserInteraction: boolean }): void => {
			if (meta.isUserInteraction) shell.setSizes(group, layout);
		};

	return (
		<div className="shell" data-testid="shell">
			<CommandBar
				panels={panels}
				layout={shell.layout}
				onAction={shell.run}
				onTogglePanel={shell.togglePanel}
				onMovePanel={shell.movePanel}
			/>
			<Toolbar status={status} version={engine.version} />

			<div className="shell__body">
				<Rail
					panels={panels}
					hidden={shell.layout.hidden}
					onToggle={shell.togglePanel}
				/>
				<Group
					className="shell__columns"
					id="columns"
					orientation="horizontal"
					defaultLayout={shell.layout.sizes["columns"]}
					onLayoutChanged={remember("columns")}
				>
					<SlotPanel
						slot="left"
						shell={shell}
						id="left"
						defaultSize={DEFAULT_LEFT}
						separator="after"
					/>
					<Panel id="centre" minSize="30%">
						<Group
							className="shell__rows"
							id="rows"
							orientation="vertical"
							defaultLayout={shell.layout.sizes["rows"]}
							onLayoutChanged={remember("rows")}
						>
							<Panel id="deck" minSize="20%">
								<Deck shell={shell} />
							</Panel>
							<SlotPanel
								slot="bottom"
								shell={shell}
								id="bottom"
								defaultSize={DEFAULT_BOTTOM}
								separator="before"
								horizontal
							/>
						</Group>
					</Panel>
					<SlotPanel
						slot="right"
						shell={shell}
						id="right"
						defaultSize={DEFAULT_RIGHT}
						separator="before"
					/>
				</Group>
			</div>

			<StatusStrip
				stats={stats}
				phase={status.phase}
				renderer={status.renderer}
				hint={shell.hint}
			/>
		</div>
	);
}

/**
 * One dockable slot, and the separator that sizes it.
 *
 * Renders nothing at all when the slot has no visible panel — a separator
 * beside an empty frame is a splitter that resizes nothing, and that is worse
 * than the frame simply not being there.
 */
function SlotPanel({
	slot,
	shell,
	id,
	defaultSize,
	separator,
	horizontal = false,
}: {
	slot: PanelSlot;
	shell: ShellState;
	id: string;
	defaultSize: string;
	/** Which side of this panel the separator sits on. */
	separator: "before" | "after";
	/** Whether the separator splits rows rather than columns. */
	horizontal?: boolean;
}): React.JSX.Element | null {
	const shown = visible(shell.layout, slot)
		.map((panelId) => panel(panelId))
		.filter((definition): definition is PanelDefinition => definition !== undefined);
	const active = activeIn(shell.layout, slot);
	if (shown.length === 0 || active === null) return null;

	const bar = (
		<Separator
			className={`handle handle--${horizontal ? "horizontal" : "vertical"}`}
			id={`separator-${slot}`}
		/>
	);

	const body = (
		<Panel
			id={id}
			defaultSize={defaultSize}
			minSize="10%"
			collapsible
			className="shell__slot"
		>
			<Dock
				slot={slot}
				panels={shown}
				active={active}
				onActivate={shell.activatePanel}
			/>
		</Panel>
	);

	return separator === "after" ? (
		<>
			{body}
			{bar}
		</>
	) : (
		<>
			{bar}
			{body}
		</>
	);
}

/**
 * The viewport deck.
 *
 * Rendered directly rather than through a Dock: it holds one panel, it has no
 * tab strip, and it must never be moved or unmounted — the engine looks its
 * canvas up by selector and a remount hands it a detached element.
 */
function Deck({ shell }: { shell: ShellState }): React.JSX.Element | null {
	const [first] = shell.layout.slots.deck;
	const definition = first === undefined ? undefined : panel(first as PanelId);
	if (definition === undefined) return null;
	return (
		/* Not data-testid="deck" — the splitter library derives a test id from
		 * a Panel's `id`, and this element sits inside the Panel called
		 * "deck". Two nodes answering to one test id is a suite that asserts
		 * on whichever it finds first. */
		<div className="deck" data-testid="viewport-deck">
			{definition.render()}
		</div>
	);
}
