// SPDX-License-Identifier: GPL-2.0-or-later
//
// The outliner: the scene as a tree you can drag (#950).
//
// ## The selection is the engine's, in both directions
//
// This panel holds no selection state and neither does the viewport. Clicking
// a row dispatches `selection.set` / `.add` / `.remove`; the engine applies it;
// the next flush answers `selection` with `{id, ids}`; the rows highlight from
// that. Picking in the viewport takes the identical path and lands in the same
// place, which is why the two agree without either of them being told.
//
// **`react-arborist` keeps a selection of its own, and this is the one place
// that has to be said out loud.** Its keyboard navigation, its ARIA and its
// drag all read `tree.selectedIds`, so it cannot be turned off without
// hand-rolling the behaviour the dependency exists to provide — and its click
// handling is already the modifier convention #950 asks for, ctrl to toggle
// one and shift for a range, which is a second thing not worth writing twice.
//
// So the library interprets the *gesture* and this file turns the result into
// commands. What the library's selection is **not** is a source of truth: the
// effect below overwrites it from the engine's answer whenever the two differ,
// in that direction only, and nothing here reads it to decide what is
// selected — the rows highlight from `useSelection()`. A selection change the
// outliner did not make — a pick, a script, an undo — repaints them, and
// `test/outliner.test.tsx` asserts exactly that.
//
// ## What is one undo step, and what is none
//
// A drag is one. `onMove` opens a gesture, dispatches a `entity.parent` per
// dragged row and commits, so the engine's ring records one entry however many
// rows moved. Delete of a multi-selection is the same shape. Selection changes
// are not undoable at all and never have been — the engine keeps them off its
// ring on purpose (#944, Q2).
//
// ## Live updates are the criterion that matters
//
// Nothing here refreshes. `useSceneTree()` is a watch on `scene.tree`, the
// engine stamps that domain's generation from a fingerprint of the world, and
// a fingerprint sees changes nobody announced — a script spawning an entity
// mid-frame moves the generation exactly as an editor create does. That is the
// half of #945 this panel proves, and it is proved by there being no code here
// that could have done it any other way.

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Tree, type NodeApi, type NodeRendererProps, type TreeApi } from "react-arborist";

import {
	useDocument,
	useDocumentState,
	useOptionalDocument,
	useSceneTree,
	useSelection,
} from "../../document/document-context.js";
import type { KruddDocument } from "../../document/document.js";
import { usePanelSize } from "./use-panel-size.js";
import {
	buildTree,
	dragRoots,
	entityId,
	isAncestor,
	matches,
	type Row,
	rowId,
} from "./tree.js";
import {
	browserStore,
	has,
	HIDDEN,
	type LayoutStore,
	LOCKED,
	readFlags,
	toggle,
	writeFlags,
} from "./view-state.js";

/** Row height in CSS pixels. Virtualization needs a number, not a rule. */
const ROW_HEIGHT = 24;

/*
 * The height used until the box has been measured.
 *
 * react-arborist derives which rows exist from a number, so a height of zero
 * renders nothing at all. In a browser the ResizeObserver answers on the first
 * frame and this is never seen; under jsdom every element measures zero, and
 * without it the component suite could only ever assert that the panel is
 * empty. What it must not become is a claim about virtualization — how many
 * rows a real 5,000-entity tree keeps in the DOM is Playwright's question.
 */
const UNMEASURED_HEIGHT = 320;

export interface OutlinerProps {
	/** Injected so a test can drive the flag mirror without touching storage. */
	store?: LayoutStore | undefined;
}

export function Outliner({ store }: OutlinerProps = {}): React.JSX.Element {
	const document = useOptionalDocument();

	if (document === null) {
		/*
		 * The same answer the inspector gives, for the same reason: this panel
		 * is entirely a view of engine state, a booting engine is not an error,
		 * and saying so beats an empty box.
		 */
		return (
			<p className="panel__empty" data-testid="outliner-waiting">
				Waiting for the engine.
			</p>
		);
	}
	return <OutlinerTree store={store} />;
}

/*
 * Split so every hook below can assume a document. The alternative is a
 * component whose hooks all begin with a null check for a state that ends
 * permanently after the first second of the session.
 */
function OutlinerTree({ store }: OutlinerProps): React.JSX.Element {
	const document = useDocument();
	const tree = useSceneTree();
	const selection = useSelection();
	/*
	 * The one thing this panel needs from the file-level state: how many
	 * documents have been opened. It keys the tree, so opening a project
	 * remounts it and the expanded rows go with the world they described.
	 */
	const { loads } = useDocumentState();

	const [box, setBox] = useState<HTMLDivElement | null>(null);
	const size = usePanelSize(box);
	const [term, setTerm] = useState("");

	const treeRef = useRef<TreeApi<Row> | null>(null);
	const rows = useMemo(() => buildTree(tree), [tree]);

	/*
	 * Set while something other than a reader is moving the library's
	 * selection, so the `onSelect` it triggers is recognised as an echo rather
	 * than as a gesture. Two things do that, and both would otherwise send a
	 * command the reader did not ask for:
	 *
	 *  - the sync effect below, pushing the engine's answer in. `setSelection`
	 *    calls `onSelect` back synchronously, before the library has
	 *    re-rendered, so what it hands over is the *previous* selection.
	 *  - the library's own mount, which deselects everything when no
	 *    `selection` prop is given. Unguarded, opening a dock with something
	 *    already selected would clear it.
	 *
	 * Hence the initial `true`: child effects run before a parent's, so the
	 * library's mount deselect happens first and is swallowed, and the first
	 * run of the effect below is what arms the flag down.
	 */
	const syncing = useRef(true);

	const selected = useMemo(() => selection?.ids ?? [], [selection]);
	/* A stable key for the effect below — `ids` is a fresh array whenever the
	 * engine re-answers, even when the answer is the same. */
	const selectedKey = selected.join(",");
	const primary = selection?.id ?? -1;

	/* ---------------------------------------------------------------- *
	 * Selection, out
	 * ---------------------------------------------------------------- */

	const applySelection = useCallback(
		(next: number[], newPrimary: number) => {
			applySelectionTo(document, next, newPrimary);
		},
		[document]
	);

	/* ---------------------------------------------------------------- *
	 * Selection, in
	 * ---------------------------------------------------------------- */

	useEffect(() => {
		const api = treeRef.current;
		/* Cleared before any early return: a first run that found the two
		 * already in agreement would otherwise leave the guard up forever and
		 * swallow every real gesture after it. */
		syncing.current = false;
		if (!api) return;

		const want = selectedKey.length > 0 ? selectedKey.split(",") : [];
		const have = [...api.selectedIds].sort();
		if (sameIds(want.slice().sort(), have)) return;

		/*
		 * The one-directional sync. The engine has answered with a selection
		 * the library's copy does not match — because a pick, a script or an
		 * undo moved it — so the library is corrected. `setSelection` fires
		 * `onSelect`, which is why that handler compares against the engine
		 * before dispatching anything: without the comparison this would echo
		 * straight back out as a command and the two would ping-pong forever.
		 */
		const primaryRow = primary >= 0 ? rowId(primary) : null;
		syncing.current = true;
		try {
			api.setSelection({
				ids: want,
				anchor: primaryRow,
				mostRecent: primaryRow,
			});
		} finally {
			syncing.current = false;
		}
		/* Scrolled to, which is the other half of "click in the viewport and
		 * the tree follows": a highlighted row a hundred rows off screen is
		 * not an answer to where the thing you picked lives. */
		if (primaryRow !== null) api.scrollTo(primaryRow, "smart");
	}, [selectedKey, primary, rows]);

	const onSelect = useCallback(
		(nodes: NodeApi<Row>[]): void => {
			/*
			 * The echo of our own push. `setSelection` calls this back
			 * synchronously, before the library's state has re-rendered, so the
			 * nodes it hands over are the *previous* selection — dispatching
			 * from that would immediately undo what the engine had just said.
			 */
			if (syncing.current) return;

			const next = nodes.map((node) => node.data.entity);
			if (sameIds([...next].sort(), [...selected].sort())) return;

			/*
			 * Every way the library can move a selection: a click, a
			 * ctrl-click, a shift-range, shift+arrow, cmd+A. All of them land
			 * here and all of them become commands — the library decides what
			 * the gesture *meant*, and the engine decides what is selected.
			 */
			const api = treeRef.current;
			const recent = api?.mostRecentNode?.data.entity;
			applySelection(next, recent ?? next[next.length - 1] ?? -1);
		},
		[applySelection, selected]
	);

	/* ---------------------------------------------------------------- *
	 * Edits
	 * ---------------------------------------------------------------- */

	const setFlags = useFlagMirror(store, rows, document, loads);

	const onMove = useCallback(
		({
			dragIds,
			parentId,
		}: {
			dragIds: string[];
			parentId: string | null;
		}): void => {
			const parent = parentId === null ? -1 : entityId(parentId);
			const moving = dragRoots(
				rows,
				dragIds.map(entityId).filter(Number.isInteger)
			);
			if (moving.length === 0) return;

			/*
			 * One gesture around the whole drag, so dropping four rows onto a
			 * folder is one press of Undo rather than four. The engine records
			 * an entry per reparent and the gesture is what folds them
			 * together — the same mechanism a gizmo drag uses (#944, Q2).
			 */
			const gesture = document.gesture(
				moving.length > 1 ? "Reparent Entities" : "Reparent Entity"
			);
			for (const id of moving) {
				document.dispatch("entity.parent", { id, parent });
			}
			gesture.commit();
		},
		[document, rows]
	);

	const onRename = useCallback(
		({ id, name }: { id: string; name: string }): void => {
			document.dispatch("entity.rename", { id: entityId(id), name });
		},
		[document]
	);

	const deleteSelection = useCallback((): void => {
		/*
		 * Deleting a parent takes its children with it, because that is what
		 * the engine's destroy does — it cascades, and it always has. Saying so
		 * is #950's criterion; changing it is not, and a delete that orphaned a
		 * subtree to the root would leave a reader with rows they did not ask
		 * to keep and no way to tell what had happened.
		 *
		 * `dragRoots` is reused to drop rows whose parent is also going: the
		 * engine would ignore the second destroy anyway, and sending it would
		 * put a dead id in the log.
		 */
		const targets = dragRoots(rows, selected);
		if (targets.length === 0) return;

		const gesture = document.gesture(
			targets.length > 1 ? "Delete Entities" : "Delete Entity"
		);
		for (const id of targets) document.dispatch("entity.destroy", { id });
		gesture.commit();
	}, [document, rows, selected]);

	const duplicateSelection = useCallback((): void => {
		const targets = dragRoots(rows, selected);
		if (targets.length === 0) return;

		const gesture = document.gesture(
			targets.length > 1 ? "Duplicate Entities" : "Duplicate Entity"
		);
		for (const id of targets) document.dispatch("entity.duplicate", { id });
		gesture.commit();
	}, [document, rows, selected]);

	/* ---------------------------------------------------------------- *
	 * Keys
	 * ---------------------------------------------------------------- */

	const onKeyDown = useCallback(
		(event: React.KeyboardEvent): void => {
			/*
			 * The shell owns every accelerator with a modifier and a panel owns
			 * unmodified keys while it has focus — the line #949 drew for the
			 * viewport, applied here unchanged. So Delete is ours and Ctrl+Z is
			 * not, and the two sets cannot overlap by construction.
			 */
			if (event.ctrlKey || event.metaKey || event.altKey) return;
			if (event.key !== "Delete") return;
			/* The library binds Backspace to its own delete handler; Delete is
			 * the key a designer reaches for and it is not taken. */
			event.preventDefault();
			deleteSelection();
		},
		[deleteSelection]
	);

	/* ---------------------------------------------------------------- *
	 * Render
	 * ---------------------------------------------------------------- */

	const disableDrop = useCallback(
		({ parentNode, dragNodes }: {
			parentNode: NodeApi<Row> | null;
			dragNodes: NodeApi<Row>[];
		}): boolean =>
			/*
			 * The visible half of the cycle refusal: no drop cursor appears
			 * over a row that is inside what is being dragged, so the reader is
			 * told before they let go. The engine refuses it as well and is the
			 * answer that counts — this tree is a frame old — but a refusal
			 * that only arrives after the drop reads as a drag that did
			 * nothing.
			 */
			parentNode !== null &&
			dragNodes.some((node) =>
				isAncestor(rows, node.data.entity, parentNode.data.entity)
			),
		[rows]
	);

	const empty = rows.length === 0;

	return (
		<section
			className="outliner"
			data-testid="outliner"
			onKeyDown={onKeyDown}
		>
			<div className="outliner__bar">
				<input
					aria-label="Filter the scene tree"
					className="outliner__search"
					data-testid="outliner-search"
					onChange={(event) => setTerm(event.target.value)}
					placeholder="Filter by name or id"
					type="search"
					value={term}
				/>
				<button
					className="outliner__action"
					data-testid="outliner-duplicate"
					disabled={selected.length === 0}
					onClick={duplicateSelection}
					type="button"
				>
					Duplicate
				</button>
				<button
					className="outliner__action"
					data-testid="outliner-delete"
					disabled={selected.length === 0}
					onClick={deleteSelection}
					type="button"
				>
					Delete
				</button>
			</div>

			<div className="outliner__body" ref={setBox}>
				{empty ? (
					<p className="panel__empty" data-testid="outliner-empty">
						This scene has no entities yet.
					</p>
				) : (
					<Tree<Row>
						/*
						 * Remounted when a different document is loaded, which
						 * is the whole of "expansion does not survive a scene
						 * load". Nothing else changes this key — a selection
						 * change must not remount the tree, or the criterion
						 * above it would fail instead.
						 */
						key={loads}
						/*
						 * Sized from the measured box rather than from CSS: the
						 * library computes which rows exist from these numbers,
						 * and a percentage would leave it rendering none.
						 */
						height={size.height || UNMEASURED_HEIGHT}
						width={size.width || "100%"}
						ref={treeRef}
						data={rows}
						idAccessor="id"
						childrenAccessor="children"
						rowHeight={ROW_HEIGHT}
						openByDefault
						searchTerm={term}
						searchMatch={(node, needle) => matches(node.data, needle)}
						disableDrop={disableDrop}
						onMove={onMove}
						onRename={onRename}
						onDelete={deleteSelection}
						onSelect={onSelect}
						aria-label="Scene tree"
						className="outliner__tree"
					>
						{(props) => (
							<OutlinerRow
								{...props}
								onToggleFlag={setFlags}
								selected={selected}
							/>
						)}
					</Tree>
				)}
			</div>

			<p className="outliner__count" data-testid="outliner-count">
				{rows.length === 0
					? "no entities"
					: `${countRows(rows)} entities · ${selected.length} selected`}
			</p>
		</section>
	);
}

/* ------------------------------------------------------------------ *
 * A row
 * ------------------------------------------------------------------ */

interface RowProps extends NodeRendererProps<Row> {
	selected: number[];
	onToggleFlag: (entity: number, from: number, bit: number) => void;
}

/*
 * A row renders; it does not handle the click. The library's own row wrapper
 * does that, because its handler is already ctrl-to-toggle and shift-for-a-
 * range against an anchor it keeps — the conventions #950 asks for. What
 * arrives here is a `data-selected` read off the engine's answer and three
 * controls that stop propagation so they are not also a selection.
 */
function OutlinerRow({
	node,
	style,
	dragHandle,
	selected,
	onToggleFlag,
}: RowProps): React.JSX.Element {
	/*
	 * The highlight comes from the engine's answer, never from
	 * `node.isSelected`. That is the whole of the "one selection, two views"
	 * rule at the point it would be easiest to break — the library has a
	 * perfectly good `isSelected` right there, and using it would make this
	 * panel render its own idea of the selection.
	 */
	const isSelected = selected.includes(node.data.entity);
	/*
	 * And the flags come from the engine's answer too, not from the browser
	 * mirror that remembers them. The mirror is memory; `scene.tree` is what is
	 * true. A toggle therefore lands a frame later, which is the same bound
	 * every other read in this editor accepts (#944, Q1) and is the price of
	 * there being one answer rather than two.
	 */
	const bits = node.data.node.flags ?? 0;
	const hidden = has(bits, HIDDEN);
	const locked = has(bits, LOCKED);

	return (
		<div
			className={[
				"outliner__row",
				isSelected ? "outliner__row--selected" : "",
				hidden ? "outliner__row--hidden" : "",
				locked ? "outliner__row--locked" : "",
			]
				.filter(Boolean)
				.join(" ")}
			data-testid={`outliner-row-${node.data.entity}`}
			data-selected={isSelected ? "true" : "false"}
			ref={dragHandle}
			style={style}
		>
			<button
				aria-label={node.isOpen ? "Collapse" : "Expand"}
				className="outliner__twisty"
				disabled={node.isLeaf}
				onClick={(event) => {
					/* Not a selection. Without this the click falls through to
					 * the row and expanding a folder would also select it. */
					event.stopPropagation();
					node.toggle();
				}}
				type="button"
			>
				{node.isLeaf ? "" : node.isOpen ? "▾" : "▸"}
			</button>

			{node.isEditing ? (
				<input
					autoFocus
					className="outliner__rename"
					data-testid={`outliner-rename-${node.data.entity}`}
					defaultValue={node.data.name}
					onBlur={() => node.reset()}
					onClick={(event) => event.stopPropagation()}
					onKeyDown={(event) => {
						if (event.key === "Escape") node.reset();
						if (event.key === "Enter") {
							node.submit(event.currentTarget.value);
						}
					}}
				/>
			) : (
				<span
					className="outliner__name"
					onDoubleClick={(event) => {
						event.stopPropagation();
						if (!locked) node.edit();
					}}
				>
					{node.data.name}
				</span>
			)}

			<button
				aria-label={hidden ? "Show" : "Hide"}
				aria-pressed={hidden}
				className="outliner__flag"
				data-testid={`outliner-hide-${node.data.entity}`}
				onClick={(event) => {
					event.stopPropagation();
					onToggleFlag(node.data.entity, bits, HIDDEN);
				}}
				type="button"
			>
				{hidden ? "◌" : "●"}
			</button>
			<button
				aria-label={locked ? "Unlock" : "Lock"}
				aria-pressed={locked}
				className="outliner__flag"
				data-testid={`outliner-lock-${node.data.entity}`}
				onClick={(event) => {
					event.stopPropagation();
					onToggleFlag(node.data.entity, bits, LOCKED);
				}}
				type="button"
			>
				{locked ? "▣" : "▢"}
			</button>
		</div>
	);
}

/* ------------------------------------------------------------------ *
 * The flag mirror
 * ------------------------------------------------------------------ */

/**
 * Hidden and locked: read from the engine, remembered in the browser.
 *
 * The engine is authoritative — `scene.tree` reports the flags each row is
 * actually at — and this hook adds exactly two things to that: it replays what
 * was remembered onto a scene the first time it sees one, and it writes back
 * whatever a reader toggles. The engine clears the flags on a scene load
 * (they are not document state), so without the replay a reload would come up
 * with everything visible; with it, the reader's view of the scene comes back.
 */
function useFlagMirror(
	store: LayoutStore | undefined,
	rows: Row[],
	document: KruddDocument,
	loads: number
): (entity: number, from: number, bit: number) => void {
	const backing = useMemo(() => store ?? browserStore(), [store]);
	/*
	 * A ref, not state. Nothing renders from the mirror — the rows read the
	 * engine — so holding it in state would re-render the tree to remember
	 * something invisible.
	 */
	const mirror = useRef(readFlags(backing));
	const replayedFor = useRef<number | null>(null);

	useEffect(() => {
		if (replayedFor.current === loads || rows.length === 0) return;
		replayedFor.current = loads;

		/*
		 * Once per document. Not on every tree change: a replay that ran again
		 * would fight a reader who had just un-hidden something and re-hide it
		 * on the next frame. Re-armed by `loads` because the engine clears the
		 * flags when it ingests a scene, so a freshly opened project needs the
		 * replay again.
		 */
		for (const [key, bits] of Object.entries(mirror.current)) {
			const id = Number.parseInt(key, 10);
			if (Number.isInteger(id) && bits !== 0) {
				document.dispatch("entity.flags", { id, flags: bits });
			}
		}
	}, [document, loads, rows.length]);

	return useCallback(
		(entity: number, from: number, bit: number): void => {
			const next = toggle(from, bit);
			mirror.current = { ...mirror.current, [entity]: next };
			writeFlags(backing, mirror.current);
			document.dispatch("entity.flags", { id: entity, flags: next });
		},
		[backing, document]
	);
}

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

/**
 * Make the engine's selection equal `next`, with `primary` as its primary.
 *
 * A clear plus one add per member, and the order matters: the engine's primary
 * is whatever was added last, so `primary` goes at the end. That is one tape
 * record per selected row, and it is bounded — a world holds at most
 * WORLD_MAX_ENTITIES entities and the tape holds far more records than that, so
 * even selecting everything fits in one flush.
 */
function applySelectionTo(
	document: KruddDocument,
	next: number[],
	primary: number
): void {
	if (next.length === 0) {
		document.dispatch("selection.clear", {});
		return;
	}

	const ordered = [...next.filter((id) => id !== primary)];
	if (next.includes(primary)) ordered.push(primary);

	const [first, ...rest] = ordered;
	if (first === undefined) return;
	document.dispatch("selection.set", { id: first });
	for (const id of rest) document.dispatch("selection.add", { id });
}

function sameIds(a: readonly (number | string)[], b: readonly (number | string)[]): boolean {
	return (
		a.length === b.length && a.every((value, index) => String(value) === String(b[index]))
	);
}

function countRows(rows: Row[]): number {
	let total = 0;
	const walk = (list: Row[]): void => {
		for (const row of list) {
			total += 1;
			if (row.children) walk(row.children);
		}
	};
	walk(rows);
	return total;
}
