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

import * as Tabs from "@radix-ui/react-tabs";

import type { ParamBlock } from "@kruddage/engine/bridge";

import {
	useDocument,
	useEntity,
	useOptionalDocument,
	useQuery,
	useSelection,
} from "../../document/document-context.js";
import { controlFor } from "./controls.js";
import { ParamControl } from "./param-control.js";

export function Inspector(): React.JSX.Element {
	const document = useOptionalDocument();
	const selection = useSelection();
	const id = selection?.id ?? -1;

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

	if (id < 0) {
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

	return <InspectorFor id={id} />;
}

/*
 * Split so the hooks below are keyed by entity: watching `entity` and
 * `entity.params` from the parent would mean the query set changes identity on
 * every selection change *inside* a render that also has to handle there being
 * no selection at all.
 */
function InspectorFor({ id }: { id: number }): React.JSX.Element {
	const entity = useEntity(id);
	const params = useQuery("entity.params", id);
	const blocks = params?.blocks ?? [];

	return (
		<Tabs.Root
			className="inspector"
			data-testid="inspector"
			/*
			 * Keyed by entity so the tab resets when the selection moves to
			 * something without that slot — otherwise selecting an entity with
			 * no material leaves the panel on a tab that no longer exists.
			 */
			key={id}
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
			</Tabs.Content>

			{blocks.map((block, index) => (
				<Tabs.Content
					className="inspector__body"
					key={block.slot}
					value={block.slot}
				>
					<Block block={block} entity={id} slot={index} />
				</Tabs.Content>
			))}
		</Tabs.Root>
	);
}

function Block({
	block,
	entity,
	slot,
}: {
	block: ParamBlock;
	entity: number;
	slot: number;
}): React.JSX.Element {
	const document = useDocument();

	if (block.fields.length === 0) {
		return (
			<p className="panel__empty">
				{block.path ?? "This asset"} declares no parameters.
			</p>
		);
	}

	return (
		<>
			<p className="inspector__source" data-testid={`block-source-${block.slot}`}>
				{block.path ?? "unknown asset"}
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
			{block.fields.map((field, index) => (
				<ParamControl
					key={field.name}
					spec={controlFor(field)}
					label={field.name}
					value={field.value}
					onGestureStart={() => {
						const gesture = document.gesture(`Set ${field.name}`);
						return gesture.commit;
					}}
					onChange={(value) =>
						document.dispatch("entity.param", {
							id: entity,
							slot,
							field: index,
							value,
						})
					}
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
