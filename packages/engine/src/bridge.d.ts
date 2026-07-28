// SPDX-License-Identifier: GPL-2.0-or-later
//
// Types for @kruddage/engine/bridge. Hand-written, like the package's other
// declaration file — see src/bridge.mjs for what any of it means.
//
// These are the shapes the C side emits, so they are a contract with
// krudd/engine/ui/bridge and not a convenience: a field renamed here without
// being renamed there is a lie the compiler will happily believe.

/** Domains a generation is kept for. Mirrors enum bridge_domain. */
export interface Generations {
	scene: number;
	selection: number;
	history: number;
}

/** A notice the generation vector cannot express on its own. */
export interface BridgeEvent {
	type: string;
	code: number;
	text: string;
}

/** One query's answer. `fresh` means the caller's cached value still stands. */
export interface BridgeResult {
	kind: string;
	/** Present only on entity queries. */
	id?: number;
	generation: number;
	fresh: boolean;
	value?: unknown;
}

/** The document the engine returns from one exchange. */
export interface BridgeReply {
	protocol: number;
	serial: number;
	applied: number;
	/** null on success; "client" when the failure never reached the engine. */
	error: string | null;
	code: number;
	generations: Generations;
	events: BridgeEvent[];
	eventsDropped: number;
	results: BridgeResult[];
}

export interface Vec3Like {
	readonly [index: number]: number;
}

export interface TransformLike {
	position?: Vec3Like;
	rotation?: Vec3Like;
	scale?: Vec3Like;
}

/** A transform as the engine reports one. */
export interface Transform {
	position: [number, number, number];
	rotation: [number, number, number, number];
	scale: [number, number, number];
}

/** A row in the scene tree. Deliberately without a transform — see the C side. */
export interface TreeNode {
	id: number;
	parent: number;
	name: string | null;
	mask: number;
	render: number;
	material: number;
	script: number;
}

export interface SceneTree {
	paused: boolean;
	entities: TreeNode[];
}

export interface EntityDetail extends TreeNode {
	local: Transform;
	world: Transform;
}

export interface SelectionState {
	id: number;
}

export interface HistoryState {
	canUndo: boolean;
	canRedo: boolean;
	undoLabel: string | null;
	redoLabel: string | null;
}

/** Query kinds, and what each answers with. */
export interface QueryValues {
	"scene.tree": SceneTree;
	entity: EntityDetail;
	selection: SelectionState;
	history: HistoryState;
}

export type QueryKind = keyof QueryValues;

/** The subset of an emscripten module this client touches. */
export interface BridgeModule {
	HEAPU8: Uint8Array;
	UTF8ToString: (ptr: number) => string;
	_krudd_bridge_protocol?: () => number;
	_krudd_bridge_buffer?: () => number;
	_krudd_bridge_capacity?: () => number;
	_krudd_bridge_exchange: (len: number) => number;
}

export interface BridgeOptions {
	/** Called with any reply carrying an error, engine-side or client-side. */
	onError?: (reply: BridgeReply) => void;
}

export declare class Tape {
	constructor(capacity?: number);
	readonly length: number;
	readonly count: number;
	reset(): this;
	open(op: number): this;
	close(): this;
	i32(value: number): this;
	u32(value: number): this;
	f32(value: number): this;
	transform(t: TransformLike | null | undefined): this;
	str(value: string): this;
	bytes(): Uint8Array;
}

export interface Bridge {
	readonly protocol: number;
	readonly capacity: number;
	readonly generations: Generations;
	readonly lastReply: BridgeReply | null;
	/** True when nothing is queued and nothing is watched. */
	readonly idle: boolean;

	/**
	 * Register interest in a query and get back an unregister function. The
	 * query is re-asked every flush; the engine answers `fresh` and sends no
	 * payload while its generation holds.
	 */
	watch<K extends QueryKind>(kind: K, id?: number): () => void;

	/** The last value the engine gave for a watched query, or null. */
	read<K extends QueryKind>(kind: K, id?: number): QueryValues[K] | null;

	beginGesture(label: string): Bridge;
	commitGesture(): Bridge;
	abortGesture(): Bridge;
	undo(): Bridge;
	redo(): Bridge;
	select(id: number): Bridge;
	setPaused(paused: boolean): Bridge;
	createEntity(
		parent: number,
		transform: TransformLike,
		mask?: number,
		renderRef?: number
	): Bridge;
	destroyEntity(id: number): Bridge;
	setTransform(id: number, transform: TransformLike): Bridge;
	setName(id: number, name: string): Bridge;
	setRenderRef(id: number, ref: number): Bridge;
	setMaterialRef(id: number, ref: number): Bridge;
	setScriptRef(id: number, ref: number): Bridge;

	subscribe(listener: (reply: BridgeReply) => void): () => void;

	/** One crossing. Null when there was nothing to send and nothing to ask. */
	flush(): BridgeReply | null;

	/** The tape the next flush would send. For tests and for debugging. */
	encode(): Tape;
}

export declare const BRIDGE_PROTOCOL: number;
export declare const TAPE_MAGIC: number;
export declare const OP: Readonly<Record<string, number>>;

export declare function createBridge(
	module: BridgeModule,
	options?: BridgeOptions
): Bridge;

/** Whether the module's wire version is one this client understands. */
export declare function protocolMatches(bridge: Bridge): boolean;
