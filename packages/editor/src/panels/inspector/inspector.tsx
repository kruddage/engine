// SPDX-License-Identifier: GPL-2.0-or-later
//
// The inspector: one panel, tabs that follow the selection, and a form nobody
// wrote.
//
// ## What is derived and what is not
//
// Everything below the tab strip is derived. The blocks come from the engine,
// the fields come from each block, and each field's control comes from its
// authored `(edit ...)` hint through controls.ts. **There is no branch in this
// file on a parameter name, an asset path or an asset type**, which is the
// property #951 exists to establish — the moment one appears, adding a
// parameter starts costing an editor PR.
//
// The Entity tab is the exception and is not a counter-example: name, parent
// and transform are the entity's own fields rather than an asset's declared
// params, they have no `(params ...)` clause anywhere to derive from, and they
// are the same three for every entity that will ever exist.
//
// ## One instance, tabs inside it
//
// #948's vocabulary: the inspector is one panel with tabs, not three docks.
// This renders one component whose tab state follows the selection, so there is
// no path that constructs a second Inspector — the failure #886's mockup hit
// when it grew three that drifted apart.
//
// ## The selection is a set
//
// Since #950 the engine answers `selection` with `{id, ids}` — a primary and
// the whole set — and this panel reads the set. What a multi-selection shows is
// the intersection of what its members declare, worked out by merge.ts, and an
// edit fans out to every member inside one gesture so it is one undo step. The
// controls below are handed one value and one `mixed` flag and know nothing
// about any of that, which is what keeps a second entity from costing the
// derivation anything.

import * as Tabs from "@radix-ui/react-tabs";

import type { SelectionState } from "@kruddage/engine/bridge";

import {
	useDocument,
	useEntity,
	useOptionalDocument,
	useQueryEach,
	useSelection,
} from "../../document/document-context.js";
import { controlFor } from "./controls.js";
import { mergeParams, type MergedBlock } from "./merge.js";
import { ParamControl } from "./param-control.js";

export function Inspector(): React.JSX.Element {
	const document = useOptionalDocument();
	const selection = useSelection();

	if (document === null) {
		/*
		 * Before the wasm runtime is up there is no document to read, and this
		 * panel is entirely a view of engine state. Saying so beats an empty
		 * box, and beats throwing — a booting engine is not an error.
		 */
		return (
			<p className="panel__empty" data-testid="inspector-waiting">
				Waiting for the engine.
			</p>
		);
	}

	/*
	 * The primary leads the list, and the rest follow in the engine's order.
	 * `ids` is authoritative when it is there; a build or a test that answers
	 * with only `id` still gets the single-entity form rather than nothing.
	 */
	const ids = selectedIds(selection);

	if (ids.length === 0) {
		/*
		 * Said plainly rather than rendered as an empty panel — #948's
		 * criterion, and the difference between "nothing is selected" and
		 * "this is broken".
		 */
		return (
			<p className="panel__empty" data-testid="inspector-empty">
				Nothing selected. Pick an entity to see its properties.
			</p>
		);
	}

	return <InspectorFor ids={ids} />;
}

/**
 * Which entities the panel is showing, primary first.
 *
 * Duplicates are dropped and the primary is pulled to the front: the engine's
 * `ids` already contains the primary, and a form whose first column depended on
 * ctrl-click order would reshuffle as a reader added rows.
 */
function selectedIds(selection: SelectionState | null): number[] {
	if (selection === null) return [];
	/* `ids` arrives from the boundary, so it is defended rather than trusted:
	 * an older engine build answers this query without it, and the single-
	 * entity form is the right thing to show when it does. */
	const rest = (selection.ids ?? []).filter((id) => id >= 0 && id !== selection.id);
	const primary = selection.id >= 0 ? [selection.id] : [];
	return [...primary, ...new Set(rest)];
}

/*
 * Split so the hooks below are keyed by the selection: watching the params of
 * every selected entity from the parent would mean the query set changes
 * identity on every selection change *inside* a render that also has to handle
 * there being no selection at all.
 */
function InspectorFor({ ids }: { ids: number[] }): React.JSX.Element {
	const primary = ids[0] ?? -1;
	const entity = useEntity(primary);
	const answers = useQueryEach("entity.params", ids);
	const { blocks, pending } = mergeParams(answers);

	return (
		<Tabs.Root
			className="inspector"
			data-testid="inspector"
			/*
			 * Keyed by the selection so the tab resets when it moves to
			 * something without that slot — otherwise selecting an entity with
			 * no material leaves the panel on a tab that no longer exists.
			 */
			key={ids.join(",")}
			defaultValue="entity"
		>
			<Tabs.List className="inspector__tabs" aria-label="Inspector sections">
				<Tabs.Trigger className="inspector__tab" value="entity">
					Entity
				</Tabs.Trigger>
				{blocks.map((block) => (
					<Tabs.Trigger
						className="inspector__tab"
						key={block.slot}
						value={block.slot}
					>
						{TAB_LABEL[block.slot] ?? block.slot}
					</Tabs.Trigger>
				))}
			</Tabs.List>

			<Tabs.Content className="inspector__body" value="entity">
				{ids.length > 1 ? (
					/*
					 * The facts below are the primary's, and they are one
					 * entity's. Saying whose beats either showing them
					 * unlabelled — which reads as facts about all of them —
					 * or hiding them, which loses the answer to "what did I
					 * just ctrl-click onto".
					 */
					<p className="inspector__source" data-testid="inspector-count">
						{ids.length} entities selected · showing the primary
					</p>
				) : null}
				{entity === null ? (
					<p className="panel__empty">This entity is no longer in the scene.</p>
				) : (
					<dl className="inspector__facts" data-testid="inspector-facts">
						<Fact label="id" value={String(entity.id)} />
						<Fact label="name" value={entity.name ?? "—"} />
						<Fact
							label="parent"
							value={entity.parent < 0 ? "root" : String(entity.parent)}
						/>
						<Fact
							label="position"
							value={entity.local.position.map(trim).join(", ")}
						/>
						<Fact
							label="scale"
							value={entity.local.scale.map(trim).join(", ")}
						/>
					</dl>
				)}
				{pending > 0 ? (
					<p className="inspector__warning" role="status">
						{pending === 1
							? "One selected entity has not answered yet."
							: `${pending} selected entities have not answered yet.`}
					</p>
				) : null}
			</Tabs.Content>

			{blocks.map((block) => (
				<Tabs.Content className="inspector__body" key={block.slot} value={block.slot}>
					<Block block={block} selected={ids.length} />
				</Tabs.Content>
			))}
		</Tabs.Root>
	);
}

function Block({
	block,
	selected,
}: {
	block: MergedBlock;
	selected: number;
}): React.JSX.Element {
	const document = useDocument();

	if (block.fields.length === 0) {
		return (
			<p className="panel__empty">
				{selected > 1 && block.unshared > 0
					? "These entities share no parameters on this asset."
					: `${block.source} declares no parameters.`}
			</p>
		);
	}

	return (
		<>
			<p className="inspector__source" data-testid={`block-source-${block.slot}`}>
				{block.source}
				{block.overridden ? "" : " · at defaults"}
			</p>
			{block.truncated ? (
				/*
				 * Never silent. A block with more fields than the boundary
				 * carries would otherwise look like an asset that simply has
				 * fewer parameters than it has.
				 */
				<p className="inspector__warning" role="status">
					This asset declares more parameters than the editor can show.
				</p>
			) : null}
			{block.unshared > 0 ? (
				/*
				 * Same rule, the other cause: a parameter only some of the
				 * selection declares is not editable together, and a panel
				 * that just left it out would read as an asset that does not
				 * have it.
				 */
				<p className="inspector__warning" role="status">
					{block.unshared === 1
						? "1 parameter is not shared by the whole selection and is hidden."
						: `${block.unshared} parameters are not shared by the whole selection and are hidden.`}
				</p>
			) : null}
			{block.fields.map((field) => (
				<ParamControl
					key={field.declaration.name}
					spec={controlFor(field.declaration)}
					label={field.declaration.name}
					value={field.value}
					mixed={field.mixed}
					onGestureStart={() => {
						const gesture = document.gesture(`Set ${field.declaration.name}`);
						return gesture.commit;
					}}
					onChange={(value) => {
						/*
						 * Every target, addressed by its own slot and field
						 * index. The engine coalesces the writes inside the
						 * open gesture, so setting a shared parameter across
						 * four entities is one undo step.
						 */
						for (const target of field.targets) {
							document.dispatch("entity.param", {
								id: target.id,
								slot: target.slot,
								field: target.field,
								value,
							});
						}
					}}
				/>
			))}
		</>
	);
}

function Fact({ label, value }: { label: string; value: string }): React.JSX.Element {
	return (
		<>
			<dt>{label}</dt>
			<dd>{value}</dd>
		</>
	);
}

const TAB_LABEL: Record<string, string> = {
	mesh: "Mesh",
	material: "Material",
	script: "Script",
};

function trim(value: number): string {
	return String(Math.round(value * 1e4) / 1e4);
}
