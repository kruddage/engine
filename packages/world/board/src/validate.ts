// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Reading a board document, and refusing one that does not hold together.
 *
 * Two jobs, deliberately separate. `parseProject` turns text into a
 * [`Project`] and is hostile-input code: a document is a file, a file can be
 * anything, and `JSON.parse` will hand back whatever it was given. `validate`
 * takes a document that is already the right *shape* and asks whether it
 * *means* anything — whether the kinds resolve, the wires land on ports that
 * exist, and the execution order is a chain rather than a guess.
 *
 * **Every failure names the node or the wire it is about.** A wire pointing
 * at a node that does not exist must not be a silent no-op: silence here
 * shows up much later as a node that mysteriously never runs, which is the
 * one thing an editor for an eight-year-old cannot afford. `validate`
 * collects every problem rather than stopping at the first, because an editor
 * wants to mark all of them at once.
 */

import type {
	Board,
	BoardColumn,
	BoardId,
	BoardNode,
	ColumnKind,
	Lane,
	NodeId,
	NodeKind,
	ParamDeclaration,
	ParamValue,
	Port,
	PortType,
	Project,
	Registry,
	Wire,
	WireId,
} from "./document";
import {
	COLUMN_KINDS,
	DOCUMENT_VERSION,
	EXEC_IN,
	EXEC_OUT,
	LANES,
	PORT_TYPES,
	paramOf,
} from "./document";
import { KINDS, kindOf } from "./kinds";

/** One thing wrong with a document, and where it is. */
export interface Problem {
	/** The board it is in, if it is in one. */
	readonly board?: BoardId;
	/** The node it is about, if it is about one. */
	readonly node?: NodeId;
	/** The wire it is about, if it is about one. */
	readonly wire?: WireId;
	/** What is wrong, as a sentence the editor can show. */
	readonly message: string;
}

/** One problem as a line of text, with the ids it is about in front. */
export function describe(problem: Problem): string {
	const where: string[] = [];
	if (problem.board !== undefined) {
		where.push(`board \`${problem.board}\``);
	}
	if (problem.node !== undefined) {
		where.push(`node \`${problem.node}\``);
	}
	if (problem.wire !== undefined) {
		where.push(`wire \`${problem.wire}\``);
	}
	return where.length === 0
		? problem.message
		: `${where.join(" ")}: ${problem.message}`;
}

/**
 * A document that could not be read.
 *
 * Carries every problem rather than only the one in the message, so a caller
 * that can show more than one line does not have to parse the text back
 * apart.
 */
export class BoardError extends Error {
	/** Everything wrong with the document. Never empty. */
	readonly problems: readonly Problem[];

	/** Wraps one or more problems. */
	constructor(problems: readonly Problem[]) {
		const lines = problems.map((p) => `  - ${describe(p)}`).join("\n");
		const count = problems.length === 1 ? "problem" : "problems";
		super(`the board document has ${problems.length} ${count}:\n${lines}`);
		this.name = "BoardError";
		this.problems = problems;
	}
}

/**
 * Everything wrong with a document, or an empty array if it holds together.
 *
 * Assumes the value is already the right shape — `parseProject` is what
 * establishes that for a document off disk. Split that way because the
 * editor's own documents are built in memory and never stop being the right
 * shape, and it would be a waste to re-derive that on every edit.
 */
export function validate(project: Project, kinds: Registry = KINDS): Problem[] {
	const problems: Problem[] = [];

	if (project.version !== DOCUMENT_VERSION) {
		problems.push({
			message:
				`the document is version ${project.version}; this engine reads ` +
				`version ${DOCUMENT_VERSION}`,
		});
	}
	if (!Object.hasOwn(project.boards, project.root)) {
		problems.push({
			message: `the project opens on board \`${project.root}\`, which it does not hold`,
		});
	}

	for (const [id, board] of Object.entries(project.boards)) {
		checkBoard(project, id, board, kinds, problems);
	}
	checkNesting(project, problems);
	return problems;
}

/** Everything wrong with one board. */
function checkBoard(
	project: Project,
	board: BoardId,
	value: Board,
	kinds: Registry,
	problems: Problem[],
): void {
	const columns = checkColumns(board, value, problems);

	const nodes = new Map<NodeId, BoardNode>();
	for (const node of value.nodes) {
		if (nodes.has(node.id)) {
			problems.push({
				board,
				node: node.id,
				message: "two nodes share this id",
			});
			continue;
		}
		nodes.set(node.id, node);
		checkNode(project, board, node, kinds, columns, problems);
	}

	const seen = new Set<WireId>();
	// One data wire may feed an input, and one execution wire may enter or
	// leave a node. Anything else leaves the order — or the value — a guess,
	// and the interpreter would have to pick one silently.
	const filled = new Set<string>();
	const entered = new Set<NodeId>();
	const left = new Set<NodeId>();

	for (const wire of value.wires) {
		if (seen.has(wire.id)) {
			problems.push({
				board,
				wire: wire.id,
				message: "two wires share this id",
			});
			continue;
		}
		seen.add(wire.id);

		const from = nodes.get(wire.from.node);
		const to = nodes.get(wire.to.node);
		if (from === undefined) {
			problems.push({
				board,
				wire: wire.id,
				message: `it leaves node \`${wire.from.node}\`, which this board does not hold`,
			});
		}
		if (to === undefined) {
			problems.push({
				board,
				wire: wire.id,
				message: `it arrives at node \`${wire.to.node}\`, which this board does not hold`,
			});
		}
		if (from === undefined || to === undefined) {
			continue;
		}

		if (wire.kind === "exec") {
			checkExecWire(board, wire, from, to, kinds, problems);
			if (left.has(from.id)) {
				problems.push({
					board,
					wire: wire.id,
					message: `node \`${from.id}\` already runs something next — execution order would be a guess`,
				});
			}
			left.add(from.id);
			if (entered.has(to.id)) {
				problems.push({
					board,
					wire: wire.id,
					message: `node \`${to.id}\` is already run by something — execution order would be a guess`,
				});
			}
			entered.add(to.id);
			continue;
		}

		checkDataWire(board, wire, from, to, kinds, problems);
		const port = `${to.id}.${wire.to.port}`;
		if (filled.has(port)) {
			problems.push({
				board,
				wire: wire.id,
				message: `\`${port}\` is already fed by another wire`,
			});
		}
		filled.add(port);
	}

	checkExecCycles(board, value, problems);
}

/**
 * Everything wrong with a board's declared columns, and the columns
 * themselves — by name, kind conflicts already resolved in favour of the
 * first declaration — for [`checkParams`] to check a column-name param
 * against.
 */
function checkColumns(
	board: BoardId,
	value: Board,
	problems: Problem[],
): ReadonlyMap<string, ColumnKind> {
	const columns = new Map<string, ColumnKind>();
	for (const column of value.columns ?? []) {
		if (columns.has(column.name)) {
			problems.push({
				board,
				message: `two columns share the name \`${column.name}\``,
			});
			continue;
		}
		columns.set(column.name, column.kind);
	}
	return columns;
}

/** Everything wrong with one node. */
function checkNode(
	project: Project,
	board: BoardId,
	node: BoardNode,
	kinds: Registry,
	columns: ReadonlyMap<string, ColumnKind>,
	problems: Problem[],
): void {
	const at = { board, node: node.id };
	const kind = kindOf(kinds, node.kind);
	if (kind === undefined) {
		problems.push({
			...at,
			message: `no kind named \`${node.kind}\` — nothing knows what this node does`,
		});
		return;
	}
	if (!LANES.includes(node.lane)) {
		problems.push({ ...at, message: `\`${node.lane}\` is not a lane` });
	}
	if (kind.entry !== undefined && node.lane !== kind.entry) {
		problems.push({
			...at,
			message: `\`${node.kind}\` starts the ${kind.entry} lane, so it cannot sit in the ${node.lane} lane`,
		});
	}
	if (!Number.isFinite(node.column) || node.column < 0) {
		problems.push({
			...at,
			message: `its column is ${node.column}; a column counts from zero`,
		});
	}
	if (node.board !== undefined) {
		if (!Object.hasOwn(project.boards, node.board)) {
			problems.push({
				...at,
				message: `it opens into board \`${node.board}\`, which the project does not hold`,
			});
		} else if (node.board === board) {
			problems.push({
				...at,
				message: "it opens into the board it sits on",
			});
		}
	}
	checkParams(at, node, kind, columns, problems);
}

/** Everything wrong with what a node sets its params to. */
function checkParams(
	at: { board: BoardId; node: NodeId },
	node: BoardNode,
	kind: NodeKind,
	columns: ReadonlyMap<string, ColumnKind>,
	problems: Problem[],
): void {
	for (const [name, value] of Object.entries(node.params ?? {})) {
		const declared = kind.params.find((p) => p.name === name);
		if (declared === undefined) {
			problems.push({
				...at,
				message: `\`${node.kind}\` has no param named \`${name}\``,
			});
			continue;
		}
		const wrong = paramProblem(declared, value);
		if (wrong !== null) {
			problems.push({ ...at, message: `param \`${name}\`: ${wrong}` });
		}
	}
	// Every column-name param at its *effective* value — the node's own, or
	// the kind's default — not only the ones a node happens to override. A
	// node that never touches its `position` param still runs against
	// whatever the board declares `position` to be, so a kind mismatch has to
	// be caught there too, not only on a node that names a column explicitly.
	for (const declared of kind.params) {
		if (declared.type !== "column-name") {
			continue;
		}
		const value = paramOf(node, kind, declared.name);
		if (typeof value === "string") {
			checkColumnNameParam(at, kind, declared.name, value, columns, problems);
		}
	}
}

/**
 * Everything wrong with a param that names a column: that the board declares
 * it, and that its kind is the one the port the param names actually feeds.
 */
function checkColumnNameParam(
	at: { board: BoardId; node: NodeId },
	kind: NodeKind,
	name: string,
	value: string,
	columns: ReadonlyMap<string, ColumnKind>,
	problems: Problem[],
): void {
	const declaredKind = columns.get(value);
	if (declaredKind === undefined) {
		problems.push({
			...at,
			message: `param \`${name}\` names column \`${value}\`, which this board does not declare`,
		});
		return;
	}
	// The port of the same name is the one this column-name param feeds, on
	// whichever side the kind has it — `integrate`'s `position` param feeds a
	// port of that name on both sides, because it reads and writes the same
	// column.
	const port = [...kind.inputs, ...kind.outputs].find((p) => p.name === name);
	const fed = port === undefined ? undefined : columnKindOf(port.type);
	if (fed !== undefined && fed !== declaredKind) {
		problems.push({
			...at,
			message:
				`param \`${name}\` names column \`${value}\`, declared as ` +
				`${declaredKind}, but the port it feeds takes column<${fed}>`,
		});
	}
}

/** The element width a `column<...>` port type carries, or `undefined`. */
function columnKindOf(type: PortType): ColumnKind | undefined {
	switch (type) {
		case "column<vec3>":
			return "vec3";
		case "column<f32>":
			return "f32";
		case "column<u32>":
			return "u32";
		default:
			return undefined;
	}
}

/** Why a value does not fit its declaration, or `null` if it does. */
function paramProblem(
	declared: ParamDeclaration,
	value: ParamValue,
): string | null {
	if (numeric(declared.type)) {
		if (typeof value !== "number" || !Number.isFinite(value)) {
			return `${JSON.stringify(value)} is not a ${declared.type}`;
		}
		if (declared.type === "u32" && !Number.isInteger(value)) {
			return `${value} is not a whole number`;
		}
		if (declared.min !== undefined && value < declared.min) {
			return `${value} is below the smallest legal value, ${declared.min}`;
		}
		if (declared.max !== undefined && value > declared.max) {
			return `${value} is above the largest legal value, ${declared.max}`;
		}
		return null;
	}
	return typeof value === "string"
		? null
		: `${JSON.stringify(value)} is not a ${declared.type}`;
}

/** Whether a param of this type takes a number rather than a name. */
function numeric(type: PortType): boolean {
	return type === "f32" || type === "u32";
}

/** Everything wrong with one execution wire. */
function checkExecWire(
	board: BoardId,
	wire: Wire,
	from: BoardNode,
	to: BoardNode,
	kinds: Registry,
	problems: Problem[],
): void {
	const at = { board, wire: wire.id };
	if (wire.from.port !== EXEC_OUT || wire.to.port !== EXEC_IN) {
		problems.push({
			...at,
			message: `an execution wire runs \`${EXEC_OUT}\` to \`${EXEC_IN}\`, not \`${wire.from.port}\` to \`${wire.to.port}\``,
		});
	}
	// A lane is when a node runs, so an execution wire across two of them
	// claims a node runs at two different times.
	if (from.lane !== to.lane) {
		problems.push({
			...at,
			message: `it runs from the ${from.lane} lane into the ${to.lane} lane`,
		});
	}
	const kind = kindOf(kinds, to.kind);
	if (kind?.entry !== undefined) {
		problems.push({
			...at,
			message: `node \`${to.id}\` is an entry point — execution starts there, so nothing runs it`,
		});
	}
}

/** Everything wrong with one data wire. */
function checkDataWire(
	board: BoardId,
	wire: Wire,
	from: BoardNode,
	to: BoardNode,
	kinds: Registry,
	problems: Problem[],
): void {
	const at = { board, wire: wire.id };
	const source = portOf(kinds, from, "outputs", wire.from.port);
	const sink = portOf(kinds, to, "inputs", wire.to.port);
	if (source === undefined) {
		problems.push({
			...at,
			message: `node \`${from.id}\` has no output named \`${wire.from.port}\``,
		});
	}
	if (sink === undefined) {
		problems.push({
			...at,
			message: `node \`${to.id}\` has no input named \`${wire.to.port}\``,
		});
	}
	if (source !== undefined && sink !== undefined && source.type !== sink.type) {
		problems.push({
			...at,
			message: `\`${from.id}.${source.name}\` carries ${source.type} and \`${to.id}.${sink.name}\` takes ${sink.type}`,
		});
	}
}

/** One of a node's ports, by name and direction. */
function portOf(
	kinds: Registry,
	node: BoardNode,
	side: "inputs" | "outputs",
	name: string,
): Port | undefined {
	return kindOf(kinds, node.kind)?.[side].find((p) => p.name === name);
}

/**
 * Execution wires that lead back to where they started.
 *
 * A ring runs forever with nothing to start it. Cheap to find here because
 * each node runs at most one thing next, so following the chain is a walk
 * rather than a search.
 */
function checkExecCycles(
	board: BoardId,
	value: Board,
	problems: Problem[],
): void {
	const next = new Map<NodeId, NodeId>();
	for (const wire of value.wires) {
		if (wire.kind === "exec" && !next.has(wire.from.node)) {
			next.set(wire.from.node, wire.to.node);
		}
	}
	const settled = new Set<NodeId>();
	for (const start of next.keys()) {
		const walked = new Set<NodeId>();
		let at: NodeId | undefined = start;
		while (at !== undefined && !settled.has(at)) {
			if (walked.has(at)) {
				problems.push({
					board,
					node: at,
					message: "execution runs in a ring back to this node",
				});
				break;
			}
			walked.add(at);
			at = next.get(at);
		}
		for (const node of walked) {
			settled.add(node);
		}
	}
}

/** Boards that open into each other, which is a breadcrumb with no top. */
function checkNesting(project: Project, problems: Problem[]): void {
	for (const [id, board] of Object.entries(project.boards)) {
		const walked = new Set<BoardId>([id]);
		let frontier = opened(board);
		while (frontier.length > 0) {
			const next: BoardId[] = [];
			for (const target of frontier) {
				if (target === id) {
					problems.push({
						board: id,
						message: "it opens into a board that opens back into it",
					});
					return;
				}
				if (walked.has(target) || !Object.hasOwn(project.boards, target)) {
					continue;
				}
				walked.add(target);
				const held = project.boards[target];
				if (held !== undefined) {
					next.push(...opened(held));
				}
			}
			frontier = next;
		}
	}
}

/** The boards this board's nodes open into. */
function opened(board: Board): BoardId[] {
	return board.nodes
		.map((node) => node.board)
		.filter((id): id is BoardId => id !== undefined);
}

/**
 * Reads a document from text.
 *
 * Throws a [`BoardError`] naming what is wrong rather than returning a
 * half-read project: a board that booted with the parts that happened to
 * parse would be a board the user is editing under a false impression of
 * what it says.
 *
 * The shape checks below are the reason this is not two lines around
 * `JSON.parse`. A document is a file and a file can hold anything — a
 * `nodes` that is a string, a `version` that is an object, a `kind` that
 * arrived from somewhere else entirely — and every one of those reaches
 * `validate` as something it would have to guess about.
 */
export function parseProject(text: string, kinds: Registry = KINDS): Project {
	let raw: unknown;
	try {
		raw = JSON.parse(text);
	} catch (cause) {
		const why = cause instanceof Error ? cause.message : String(cause);
		throw new BoardError([{ message: `it is not valid JSON: ${why}` }]);
	}
	const project = asProject(raw);
	const problems = validate(project, kinds);
	if (problems.length > 0) {
		throw new BoardError(problems);
	}
	return project;
}

/**
 * Writes a document back out.
 *
 * Fields in a fixed order and boards sorted by id, so that saving a project
 * twice produces the same bytes and a diff shows the edit rather than
 * whatever order the keys happened to be built in.
 */
export function serializeProject(project: Project): string {
	const boards: Record<string, unknown> = {};
	for (const id of Object.keys(project.boards).sort()) {
		const board = project.boards[id];
		if (board !== undefined) {
			boards[id] = {
				title: board.title,
				...(board.lanes === undefined ? {} : { lanes: board.lanes }),
				...(board.pro === undefined ? {} : { pro: board.pro }),
				...(board.columns === undefined
					? {}
					: { columns: board.columns.map(plainColumn) }),
				nodes: board.nodes.map(plainNode),
				wires: board.wires.map(plainWire),
			};
		}
	}
	const plain = { version: project.version, root: project.root, boards };
	return `${JSON.stringify(plain, null, "\t")}\n`;
}

/** One node with its fields in the document's order. */
function plainNode(node: BoardNode): Record<string, unknown> {
	return {
		id: node.id,
		kind: node.kind,
		lane: node.lane,
		column: node.column,
		...(node.params === undefined ? {} : { params: node.params }),
		...(node.board === undefined ? {} : { board: node.board }),
	};
}

/** One wire with its fields in the document's order. */
function plainWire(wire: Wire): Record<string, unknown> {
	return {
		id: wire.id,
		from: { node: wire.from.node, port: wire.from.port },
		to: { node: wire.to.node, port: wire.to.port },
		kind: wire.kind,
		...(wire.cut === undefined ? {} : { cut: wire.cut }),
	};
}

/** One column declaration with its fields in the document's order. */
function plainColumn(column: BoardColumn): Record<string, unknown> {
	return { name: column.name, kind: column.kind };
}

/**
 * The shape checks.
 *
 * Each throws on the first thing that is not what it says it is, naming the
 * path it got to — `board \`frame\` node \`camera\`: \`column\` must be a
 * number`. Collecting these the way `validate` collects its own would mean
 * carrying on past a `nodes` that turned out to be a number, and there is
 * nothing further to say about a document once that is true.
 */
function asProject(value: unknown): Project {
	const root = asRecord(value, {});
	const version = root.version;
	if (typeof version !== "number" || !Number.isInteger(version)) {
		throw one({}, "`version` must be a whole number");
	}
	const openOn = root.root;
	if (typeof openOn !== "string") {
		throw one({}, "`root` must name the board the project opens on");
	}
	const rawBoards = asRecord(root.boards, {}, "`boards`");
	const boards: Record<BoardId, Board> = {};
	for (const id of Object.keys(rawBoards)) {
		boards[id] = asBoard(rawBoards[id], id);
	}
	return { version, root: openOn, boards };
}

/** One board, checked. */
function asBoard(value: unknown, board: BoardId): Board {
	const raw = asRecord(value, { board });
	if (typeof raw.title !== "string") {
		throw one({ board }, "`title` must be text");
	}
	if (!Array.isArray(raw.nodes)) {
		throw one({ board }, "`nodes` must be a list");
	}
	if (!Array.isArray(raw.wires)) {
		throw one({ board }, "`wires` must be a list");
	}
	if (raw.pro !== undefined && typeof raw.pro !== "boolean") {
		throw one({ board }, "`pro` must be true or false");
	}
	if (raw.columns !== undefined && !Array.isArray(raw.columns)) {
		throw one({ board }, "`columns` must be a list");
	}
	return {
		title: raw.title,
		...(raw.lanes === undefined ? {} : { lanes: asLanes(raw.lanes, board) }),
		...(raw.pro === undefined ? {} : { pro: raw.pro }),
		...(raw.columns === undefined
			? {}
			: { columns: raw.columns.map((column) => asColumn(column, board)) }),
		nodes: raw.nodes.map((node) => asNode(node, board)),
		wires: raw.wires.map((wire) => asWire(wire, board)),
	};
}

/** A board's lane labels, checked — one string per lane, all three required. */
function asLanes(
	value: unknown,
	board: BoardId,
): Readonly<Record<Lane, string>> {
	const raw = asRecord(value, { board }, "`lanes`");
	const labels: Partial<Record<Lane, string>> = {};
	for (const lane of LANES) {
		const held = raw[lane];
		if (typeof held !== "string") {
			throw one({ board }, `\`lanes.${lane}\` must be text`);
		}
		labels[lane] = held;
	}
	// All three, because a board that labelled two of its lanes and left the
	// third saying something about the game would be worse than one that
	// labelled none.
	return labels as Record<Lane, string>;
}

/** One column declaration, checked. */
function asColumn(value: unknown, board: BoardId): BoardColumn {
	const raw = asRecord(value, { board }, "a column");
	if (typeof raw.name !== "string") {
		throw one({ board }, "every column needs a `name`");
	}
	if (typeof raw.kind !== "string" || !isColumnKind(raw.kind)) {
		throw one(
			{ board },
			`column \`${raw.name}\`: \`kind\` must be one of ${COLUMN_KINDS.join(", ")}`,
		);
	}
	return { name: raw.name, kind: raw.kind };
}

/** One node, checked. */
function asNode(value: unknown, board: BoardId): BoardNode {
	const raw = asRecord(value, { board }, "a node");
	if (typeof raw.id !== "string") {
		throw one({ board }, "every node needs an `id`");
	}
	const at = { board, node: raw.id };
	if (typeof raw.kind !== "string") {
		throw one(at, "`kind` must name a node kind");
	}
	if (typeof raw.lane !== "string" || !isLane(raw.lane)) {
		throw one(at, `\`lane\` must be one of ${LANES.join(", ")}`);
	}
	if (typeof raw.column !== "number") {
		throw one(at, "`column` must be a number");
	}
	return {
		id: raw.id,
		kind: raw.kind,
		lane: raw.lane,
		column: raw.column,
		...(raw.params === undefined ? {} : { params: asParams(raw.params, at) }),
		...(raw.board === undefined ? {} : { board: asBoardId(raw.board, at) }),
	};
}

/** One node's params, checked as far as their shape goes. */
function asParams(
	value: unknown,
	at: { board: BoardId; node: NodeId },
): Record<string, ParamValue> {
	const raw = asRecord(value, at, "`params`");
	const params: Record<string, ParamValue> = {};
	for (const [name, held] of Object.entries(raw)) {
		if (typeof held !== "number" && typeof held !== "string") {
			throw one(at, `param \`${name}\` must be a number or a name`);
		}
		params[name] = held;
	}
	return params;
}

/** The board a node opens into, checked. */
function asBoardId(
	value: unknown,
	at: { board: BoardId; node: NodeId },
): BoardId {
	if (typeof value !== "string") {
		throw one(at, "`board` must name the board this node opens into");
	}
	return value;
}

/** One wire, checked. */
function asWire(value: unknown, board: BoardId): Wire {
	const raw = asRecord(value, { board }, "a wire");
	if (typeof raw.id !== "string") {
		throw one({ board }, "every wire needs an `id`");
	}
	const at = { board, wire: raw.id };
	if (raw.kind !== "exec" && raw.kind !== "data") {
		throw one(at, "`kind` must be `exec` or `data`");
	}
	if (raw.cut !== undefined && typeof raw.cut !== "boolean") {
		throw one(at, "`cut` must be true or false");
	}
	return {
		id: raw.id,
		from: asEndpoint(raw.from, at, "from"),
		to: asEndpoint(raw.to, at, "to"),
		kind: raw.kind,
		...(raw.cut === undefined ? {} : { cut: raw.cut }),
	};
}

/** One end of a wire, checked. */
function asEndpoint(
	value: unknown,
	at: { board: BoardId; wire: WireId },
	side: string,
): { node: NodeId; port: string } {
	const raw = asRecord(value, at, `\`${side}\``);
	if (typeof raw.node !== "string") {
		throw one(at, `\`${side}.node\` must name a node`);
	}
	if (typeof raw.port !== "string") {
		throw one(at, `\`${side}.port\` must name a port`);
	}
	return { node: raw.node, port: raw.port };
}

/**
 * A value as a plain object.
 *
 * `null` is an object to `typeof` and an array is one too, so both are
 * checked for by hand. Built with a null prototype so that a document holding
 * a `__proto__` key cannot reach anything but its own contents.
 */
function asRecord(
	value: unknown,
	at: Omit<Problem, "message">,
	what = "the document",
): Record<string, unknown> {
	if (typeof value !== "object" || value === null || Array.isArray(value)) {
		throw one(at, `${what} must be an object`);
	}
	return Object.assign(Object.create(null), value) as Record<string, unknown>;
}

/** Whether a string is one of the three lanes. */
function isLane(value: string): value is (typeof LANES)[number] {
	return (LANES as readonly string[]).includes(value);
}

/** Whether a string is one of the three column kinds. */
function isColumnKind(value: string): value is ColumnKind {
	return (COLUMN_KINDS as readonly string[]).includes(value);
}

/** One problem, as the error to throw. */
function one(at: Omit<Problem, "message">, message: string): BoardError {
	return new BoardError([{ ...at, message }]);
}

/** Whether a string names a port type. Exported for the editor's use. */
export function isPortType(value: string): value is PortType {
	return (PORT_TYPES as readonly string[]).includes(value);
}
