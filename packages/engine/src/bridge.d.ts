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
	/**
	 * The viewport's own state (#949). Deliberately not the camera — see
	 * the `viewport` query in bridge.mjs for why the pose is left out.
	 */
	viewport: number;
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

/** One editable parameter, as the asset declared it. */
export interface ParamField {
	name: string;
	/** "float" | "int" | "vec2" | "vec3" | "vec4". */
	type: string;
	components: number;
	/**
	 * The authored `(edit ...)` hint: "none", "color" or "range". The whole
	 * of the inspector's derivation turns on this one string, and it must
	 * never be inferred from the name or the type.
	 */
	edit: string;
	min: number;
	max: number;
	/** Current value, one entry per component. */
	value: number[];
}

export interface ParamBlock {
	/** "mesh" | "material" | "script". */
	slot: string;
	asset: number;
	path: string | null;
	size: number;
	/** False means every value is a declared default, not a set one. */
	overridden: boolean;
	/** True when the declaration had more fields than the boundary carries. */
	truncated: boolean;
	fields: ParamField[];
}

export interface EntityParams {
	id: number;
	blocks: ParamBlock[];
}

/** Which handle set the gizmo shows. Mirrors enum gizmo_mode. */
export type GizmoMode = 0 | 1 | 2 | 3;

/** Which handle a drag holds. -1 is none; 3 is the uniform-scale centre. */
export type GizmoAxis = -1 | 0 | 1 | 2 | 3;

export interface GizmoSnap {
	/** World units. 0 means translation does not snap. */
	translate: number;
	/** Degrees. 0 means rotation does not snap. */
	rotate: number;
	/** A multiple of the axis's scale. 0 means scaling does not snap. */
	scale: number;
}

export interface GridState {
	shown: boolean;
	spacing: number;
}

/** The viewport, as the engine reports it. */
export interface ViewportState {
	/** The size the engine answers a pick against, in CSS pixels. */
	width: number;
	height: number;
	/** Which half of the GAME / EDITOR switch is lit. */
	editorMode: boolean;
	mode: GizmoMode;
	/** The handle a live drag holds, or -1. */
	axis: GizmoAxis;
	/**
	 * How many drags have been opened.
	 *
	 * The editor's "answered" signal. A press that grabs nothing changes
	 * no other field, so without this there is no way to tell it apart
	 * from a press the engine has not seen yet — and the editor has to
	 * know which before it can decide whether the gesture is a gizmo drag
	 * or a camera orbit.
	 */
	dragSerial: number;
	snap: GizmoSnap;
	grid: GridState;
}

/** Query kinds, and what each answers with. */
export interface QueryValues {
	"scene.tree": SceneTree;
	entity: EntityDetail;
	selection: SelectionState;
	history: HistoryState;
	/**
	 * The whole world as a `(scene ...)` form, or null when this build
	 * cannot write one. Asked through ask(), never watched — see there.
	 */
	"scene.text": string | null;
	"entity.params": EntityParams;
	viewport: ViewportState;
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

	/**
	 * Ask a query once and resolve with its answer on the next flush.
	 *
	 * The cold-path escape hatch: use it for anything a reader asks for by
	 * pressing a key, and watch() for anything a panel renders. Rejects if
	 * the flush it rode on never reached the engine.
	 */
	ask<K extends QueryKind>(kind: K, id?: number): Promise<QueryValues[K] | null>;

	beginGesture(label: string): Bridge;
	commitGesture(): Bridge;
	abortGesture(): Bridge;
	undo(): Bridge;
	redo(): Bridge;
	select(id: number): Bridge;
	setPaused(paused: boolean): Bridge;
	/**
	 * Set one field of one parameter block. `slot` indexes the blocks in the
	 * order `entity.params` reports them; `value` is up to four components.
	 */
	setParam(
		id: number,
		slot: number,
		field: number,
		value: readonly number[]
	): Bridge;
	/** Replace the document with a `(scene ...)` form. Clears the history. */
	loadScene(text: string): Bridge;
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

	/* The viewport (#949). See bridge.mjs for what each of these means. */

	/** The canvas's drawable size, CSS pixels. What a pick is answered against. */
	setViewportSize(width: number, height: number): Bridge;
	/** Orbit about the camera's target, radians. */
	orbitCamera(dyaw: number, dpitch: number): Bridge;
	/** Slide eye and target across the view plane, in viewport fractions. */
	panCamera(dx: number, dy: number): Bridge;
	/** Move along the view direction, as a fraction of the eye→target gap. */
	dollyCamera(amount: number): Bridge;
	/** Frame an entity, or the selection when `id` is -1. */
	frameCamera(id?: number): Bridge;
	/** Hand the camera back to the scene's scripted one. */
	resetCamera(): Bridge;
	/** Pick at a pixel and select what is there. A miss clears. */
	pick(x: number, y: number): Bridge;
	/** The GAME / EDITOR switch — the whole of the play/edit handover. */
	setEditorMode(on: boolean): Bridge;
	setGizmoMode(mode: GizmoMode): Bridge;
	/** Snap increments. Zero on any of them means that mode is free. */
	setGizmoSnap(
		translate: number,
		rotateDegrees: number,
		scale: number
	): Bridge;
	/** One phase of a gizmo drag, at a pixel. */
	gizmoDrag(phase: DragPhase, x: number, y: number): Bridge;
	setGrid(shown: boolean, spacing: number): Bridge;

	subscribe(listener: (reply: BridgeReply) => void): () => void;

	/** One crossing. Null when there was nothing to send and nothing to ask. */
	flush(): BridgeReply | null;

	/** The tape the next flush would send. For tests and for debugging. */
	encode(): Tape;
}

export declare const BRIDGE_PROTOCOL: number;
export declare const TAPE_MAGIC: number;
/**
 * The opcodes, named.
 *
 * Spelled out rather than `Record<string, number>` so a caller gets a type
 * error for `OP.ENTITY_RENMAE` instead of `undefined` on the wire — and so
 * that under noUncheckedIndexedAccess an opcode is a `number` rather than a
 * `number | undefined` every call site has to narrow.
 */
export declare const OP: Readonly<{
	GESTURE_BEGIN: number;
	GESTURE_COMMIT: number;
	GESTURE_ABORT: number;
	UNDO: number;
	REDO: number;
	SELECT: number;
	ENTITY_CREATE: number;
	ENTITY_DESTROY: number;
	ENTITY_TRANSFORM: number;
	ENTITY_NAME: number;
	ENTITY_RENDER_REF: number;
	ENTITY_MATERIAL_REF: number;
	ENTITY_SCRIPT_REF: number;
	SET_PAUSED: number;
	SCENE_LOAD: number;
	QUERY_TREE: number;
	QUERY_ENTITY: number;
	QUERY_SELECTION: number;
	QUERY_HISTORY: number;
	QUERY_SCENE_TEXT: number;
	VIEWPORT_SIZE: number;
	CAMERA_ORBIT: number;
	CAMERA_PAN: number;
	CAMERA_DOLLY: number;
	CAMERA_FRAME: number;
	CAMERA_RESET: number;
	PICK: number;
	EDITOR_MODE: number;
	GIZMO_MODE: number;
	GIZMO_SNAP: number;
	GIZMO_DRAG: number;
	GRID: number;
	QUERY_VIEWPORT: number;
}>;

/** The phases of a gizmo drag. Mirrors enum bridge_drag_phase. */
export type DragPhase = 0 | 1 | 2;

export declare const GIZMO_MODE: Readonly<{
	NONE: 0;
	TRANSLATE: 1;
	ROTATE: 2;
	SCALE: 3;
}>;

export declare const GIZMO_AXIS: Readonly<{
	NONE: -1;
	X: 0;
	Y: 1;
	Z: 2;
	ALL: 3;
}>;

export declare const DRAG_PHASE: Readonly<{
	BEGIN: 0;
	MOVE: 1;
	END: 2;
}>;

export declare function createBridge(
	module: BridgeModule,
	options?: BridgeOptions
): Bridge;

/** Whether the module's wire version is one this client understands. */
export declare function protocolMatches(bridge: Bridge): boolean;
