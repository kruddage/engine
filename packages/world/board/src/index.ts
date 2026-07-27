// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The board document: what a krudd project is, as data.
 *
 * **Tier: `world`.** A board is the scene data model, so it sits beside
 * entity storage rather than above it. It imports nothing from a higher tier
 * — in particular nothing from `ui`, because the board is the source of truth
 * and the board view is one way of looking at it, not the other way round.
 *
 * ## What is here
 *
 * - `document` — the types. What a board, a node, a wire, a port and a param
 *   are.
 * - `kinds` — the node kinds the triangles project is made of, with the
 *   engine's own constants as their defaults.
 * - `triangles` — that project, checked in.
 * - `validate` — reading a document off disk, and refusing one that does not
 *   hold together, by name.
 * - `file` — what a project is called when it is saved, and what it is saved
 *   as. The bytes are `validate`'s; this is the name around them.
 *
 * ## What is deliberately not here
 *
 * Running a document. A kind here declares its shape — its ports, its params
 * and their defaults — and says nothing about what it does when the frame
 * comes round. That seam is on purpose: what a kind resolves to at runtime is
 * the interpreter's question, and the answer has to accommodate a node whose
 * kind is hand-written TypeScript rather than a wiring of other nodes.
 */

export type {
	Board,
	BoardColumn,
	BoardId,
	BoardNode,
	ColumnKind,
	Endpoint,
	KindName,
	Lane,
	NodeId,
	NodeKind,
	ParamDeclaration,
	ParamValue,
	Port,
	PortName,
	PortType,
	Project,
	Registry,
	Wire,
	WireId,
	WireKind,
} from "./document";
export {
	COLUMN_KINDS,
	DOCUMENT_VERSION,
	EXEC_IN,
	EXEC_OUT,
	LANES,
	PORT_TYPES,
	paramOf,
} from "./document";
export {
	cloneProject,
	drivesTheGame,
	setParam,
	toggleWire,
} from "./edit";
export {
	DEFAULT_PROJECT_NAME,
	PROJECT_EXTENSION,
	PROJECT_MEDIA_TYPE,
	projectFileName,
} from "./file";
export { KINDS, kindOf } from "./kinds";
export { Runner } from "./run";
export { FRAME_BOARD, PAINT_TO_DRAW, ROOT_BOARD, TRIANGLES } from "./triangles";
export type { Problem } from "./validate";
export {
	BoardError,
	describe,
	isPortType,
	parseProject,
	serializeProject,
	validate,
} from "./validate";
export type { PointerFrame, Run, RunContext, WorldView } from "./world";
export { NO_POINTER } from "./world";
