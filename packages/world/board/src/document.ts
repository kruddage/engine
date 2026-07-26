// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * What a board is, as data.
 *
 * A project is a **board**: a directed graph of **nodes** joined by **wires**,
 * which attach at **ports**. Boards nest — a node may carry one — and the
 * board is the source of truth rather than a view over something else. That
 * vocabulary is the whole list; nothing here borrows a name from another
 * product.
 *
 * ## The load-bearing rule
 *
 * **A node takes a column, not an entity.** It is visible in the type system:
 * the data ports that carry world state are `column<vec3>`, not `vec3`. A
 * graph whose nodes took one entity each would execute once per object, which
 * is the per-entity boundary crossing `docs/boundary.md` measures at ~31× and
 * the engine exists to avoid. `integrate` takes the position and velocity
 * columns and walks them, once per frame.
 *
 * ## Kinds, and what a node stores
 *
 * A node's `kind` is a string resolved through a [`Registry`]. The kind owns
 * everything shared by every node of that kind — its title, its ports, and
 * the declaration and default of each param. The node owns only what is
 * particular to it: where it sits, which params it overrides, and which board
 * it opens into. So a param absent from a node is not unset, it is the kind's
 * default, and adding a param to a kind does not have to touch a single saved
 * document.
 *
 * What a kind resolves to at *runtime* is deliberately not here — that is the
 * interpreter's, and the escape hatch it has to accommodate is decided
 * separately. This module describes shape.
 *
 * ## Coordinates
 *
 * A node stores a `lane` and a `column`, not a position in pixels. The lane
 * is *when it runs*; the column is *where in that lane it sits*. Pixels are
 * the view's business precisely because there is more than one view of the
 * same board: lanes flow across a wide screen and stack down a narrow one,
 * and a document that stored `x` and `y` would be a document that had already
 * chosen one of them.
 */

/**
 * The document schema this package reads and writes.
 *
 * Bumped when a change would make an older reader wrong rather than merely
 * incomplete. A document from the future is refused by name — see
 * `validate` — because loading half of one and booting into a broken board is
 * the failure the version exists to prevent.
 */
export const DOCUMENT_VERSION = 1;

/** The id of a board within a project. Unique across the project. */
export type BoardId = string;

/** The id of a node. Unique within its board, not across the project. */
export type NodeId = string;

/** The id of a wire. Unique within its board. */
export type WireId = string;

/** The name of a node kind, resolved through a [`Registry`]. */
export type KindName = string;

/** The name of a port on a node. */
export type PortName = string;

/**
 * The port an execution wire leaves a node by.
 *
 * Reserved: every kind has one implicitly, and no kind may declare a data
 * port of this name. Execution wires name their ports like data wires do so
 * that there is one wire shape rather than two — the view draws every wire
 * between two ports, and the validator checks every wire the same way.
 */
export const EXEC_OUT = "out";

/** The port an execution wire arrives at. Reserved, like [`EXEC_OUT`]. */
export const EXEC_IN = "in";

/**
 * The three entry points, and so the three lanes: once when the board opens,
 * every frame, and every frame to the screen.
 *
 * What a lane *means* depends on the board you are standing in — inside a
 * frame these are stages of that frame rather than of the game — so the label
 * is the view's, and only the order is here.
 */
export const LANES = ["start", "step", "paint"] as const;

/** When a node runs. */
export type Lane = (typeof LANES)[number];

/**
 * What a port carries.
 *
 * `column<vec3>` is the one that matters and the reason this is a closed set:
 * world state moves between nodes a whole column at a time, and a port typed
 * `vec3` would be a port that had quietly gone per-entity.
 *
 * The names are the boundary's names on purpose. The day a node kind is
 * generated from an engine export rather than written out here, the port
 * types already line up.
 */
export const PORT_TYPES = [
	"f32",
	"u32",
	"vec3",
	"mat4",
	"column<vec3>",
	"frame",
	"surface",
	"mesh",
	"color",
] as const;

/** What a port carries. */
export type PortType = (typeof PORT_TYPES)[number];

/** One data port on a node kind. */
export interface Port {
	/** What a wire names it by. */
	readonly name: PortName;
	/** What it carries. */
	readonly type: PortType;
}

/**
 * A param's value.
 *
 * Numbers and strings only, which is what survives a JSON round trip without
 * a codec. A param that wants more structure is a data port instead.
 */
export type ParamValue = number | string;

/** One param a kind declares, and what a node gets if it does not say. */
export interface ParamDeclaration {
	/** What a node names it by. */
	readonly name: string;
	/** What kind of value it takes. */
	readonly type: PortType;
	/** The value a node that does not override it gets. */
	readonly default: ParamValue;
	/** The smallest legal value, for a numeric param. Inclusive. */
	readonly min?: number;
	/** The largest legal value, for a numeric param. Inclusive. */
	readonly max?: number;
}

/**
 * One kind of node: everything true of every node of that kind.
 *
 * Held apart from the node so that a document stores the difference rather
 * than the whole — see the module docs.
 */
export interface NodeKind {
	/** What the node box shows. */
	readonly title: string;
	/**
	 * The lane this kind is the entry point of, if it is one.
	 *
	 * An entry point is where execution starts, so nothing may wire *into*
	 * it, and a node of an entry kind must sit in the lane it starts.
	 */
	readonly entry?: Lane;
	/** The data ports wires may arrive at. */
	readonly inputs: readonly Port[];
	/** The data ports wires may leave by. */
	readonly outputs: readonly Port[];
	/** The params a node of this kind may set. */
	readonly params: readonly ParamDeclaration[];
}

/**
 * Every kind a document may name.
 *
 * A plain map because resolution is a lookup and nothing more: a kind the
 * registry does not hold is a validation failure naming the node, not a
 * silently skipped node.
 */
export type Registry = Readonly<Record<KindName, NodeKind>>;

/**
 * One node on a board.
 *
 * Named `BoardNode` rather than `Node` because `Node` is the DOM's, and the
 * board view is HTML — the two would be in scope together at exactly the
 * place the confusion would cost the most.
 */
export interface BoardNode {
	/** Unique within the board. */
	readonly id: NodeId;
	/** Resolved through the [`Registry`]. */
	readonly kind: KindName;
	/** When it runs. */
	readonly lane: Lane;
	/** Where in the lane it sits, counting from zero. */
	readonly column: number;
	/**
	 * The params this node overrides. Anything absent takes the kind's
	 * default.
	 */
	readonly params?: Readonly<Record<string, ParamValue>>;
	/**
	 * The board opening this node descends into, if it carries one.
	 *
	 * A property of the node rather than of its kind: "open this node" is one
	 * gesture, and what it lands on is this board's business.
	 */
	readonly board?: BoardId;
}

/** What a wire carries: execution order, or data. */
export type WireKind = "exec" | "data";

/** One end of a wire. */
export interface Endpoint {
	/** The node it attaches to, in the same board as the wire. */
	readonly node: NodeId;
	/**
	 * The port it attaches at.
	 *
	 * By name, never by index: an index shifts the day a kind gains a port,
	 * which would silently rewire every saved document. A name that no longer
	 * resolves fails by name instead.
	 */
	readonly port: PortName;
}

/** One connection between two nodes. */
export interface Wire {
	/** Unique within the board. */
	readonly id: WireId;
	/** Where it leaves. */
	readonly from: Endpoint;
	/** Where it arrives. */
	readonly to: Endpoint;
	/** Execution order, or data. */
	readonly kind: WireKind;
}

/** One canvas of nodes. */
export interface Board {
	/** What the breadcrumb shows. */
	readonly title: string;
	/** Its nodes, in no particular order — `lane` and `column` place them. */
	readonly nodes: readonly BoardNode[];
	/** Its wires. */
	readonly wires: readonly Wire[];
}

/**
 * A project: every board in it, and the one it opens on.
 *
 * Boards are held in a flat map keyed by id rather than nested inside the
 * nodes that open them. Nesting would make a board's identity positional —
 * "the board inside the node inside the board" — and descending into one a
 * tree walk rather than a lookup, for no gain: the nesting is already
 * described by the `board` on each node.
 */
export interface Project {
	/** The schema version. See [`DOCUMENT_VERSION`]. */
	readonly version: number;
	/** The board the project opens on. */
	readonly root: BoardId;
	/** Every board, by id. */
	readonly boards: Readonly<Record<BoardId, Board>>;
}

/**
 * What a node's param is set to: its own value, or its kind's default.
 *
 * The one place that knows a missing param is a default rather than nothing,
 * so that the interpreter and the settings sheet cannot disagree about what a
 * node is doing. Returns `undefined` only when the kind does not declare the
 * param at all, which `validate` refuses.
 */
export function paramOf(
	node: BoardNode,
	kind: NodeKind,
	name: string,
): ParamValue | undefined {
	const set = node.params?.[name];
	if (set !== undefined) {
		return set;
	}
	return kind.params.find((p) => p.name === name)?.default;
}
