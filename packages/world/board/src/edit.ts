// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Editing a board.
 *
 * **The board is the source of truth**, so editing the board edits the project
 * — there is nothing else underneath for a change to fail to reach. These are
 * the operations that make that literally true: cutting a wire, and setting a
 * param.
 *
 * ## Why the document is mutated rather than replaced
 *
 * The interpreter holds the project by reference and rebuilds its chains on
 * `reload()`. A copy-on-write edit would leave the runner walking the document
 * as it was before the edit, and the fix for that — handing the new one back to
 * everything holding the old one — is a great deal of machinery to buy an
 * immutability nothing here needs. So an edit changes the document in place and
 * the holder reloads.
 *
 * The types are `readonly` all the same, because *most* code has no business
 * changing a document. This module is the exception, the casts are here and
 * nowhere else, and that is the point of it being a module.
 *
 * ## A cut is a state, not a deletion
 *
 * Tapping a wire cuts it; tapping again reconnects it. A cut that deleted the
 * wire would have thrown away the fact that there was one, and reconnecting
 * would mean inventing it back. So `cut` is a flag: the interpreter skips it,
 * the view draws it as cut, and the document remembers.
 */

import type {
	BoardId,
	NodeId,
	ParamValue,
	Project,
	Registry,
	Wire,
	WireId,
} from "./document";
import { KINDS, kindOf } from "./kinds";

/** A deep, mutable copy of a project. */
export function cloneProject(project: Project): Project {
	// Structured cloning through JSON, which is exactly what a document is
	// made of — numbers, strings, arrays and plain objects, with no cycles by
	// construction because a nested board is an id rather than a reference.
	// A fixture that some other test mutated is a fixture nobody can trust.
	return JSON.parse(JSON.stringify(project)) as Project;
}

/**
 * Cuts a wire, or reconnects one that was cut. Returns its new state.
 *
 * `undefined` when the board or the wire is not there, which is a caller
 * naming something that does not exist rather than a wire that refused.
 */
export function toggleWire(
	project: Project,
	board: BoardId,
	wire: WireId,
): boolean | undefined {
	const held = wireOf(project, board, wire);
	if (held === undefined) {
		return undefined;
	}
	const cut = held.cut !== true;
	(held as { cut?: boolean }).cut = cut;
	return cut;
}

/**
 * Whether cutting this wire would change what the running game does.
 *
 * **Only wires that genuinely drive the running game are cuttable.** A control
 * that looks live and changes nothing teaches the wrong thing about what a
 * board is — and on a board the interpreter never runs, or on a data wire the
 * interpreter does not read, a cut would be exactly that.
 */
export function drivesTheGame(
	project: Project,
	board: BoardId,
	wire: WireId,
	kinds: Registry = KINDS,
): boolean {
	if (board !== project.root) {
		return false;
	}
	const held = wireOf(project, board, wire);
	if (held?.kind !== "exec") {
		return false;
	}
	// And only if the node it runs is one the interpreter can run at all.
	const target = boardOf(project, board)?.nodes.find(
		(node) => node.id === held.to.node,
	);
	return target !== undefined && kindOf(kinds, target.kind)?.run !== undefined;
}

/**
 * Sets a param on a node. Returns whether it landed.
 *
 * Refuses a param the kind does not declare, rather than adding one nothing
 * will ever read — `validate` would refuse the document afterwards, and an
 * edit that quietly breaks the board it is editing is worse than one that
 * does nothing.
 */
export function setParam(
	project: Project,
	board: BoardId,
	node: NodeId,
	name: string,
	value: ParamValue,
	kinds: Registry = KINDS,
): boolean {
	const held = boardOf(project, board)?.nodes.find(
		(candidate) => candidate.id === node,
	);
	const kind = held === undefined ? undefined : kindOf(kinds, held.kind);
	if (held === undefined || kind === undefined) {
		return false;
	}
	if (!kind.params.some((param) => param.name === name)) {
		return false;
	}
	const params = (held as { params?: Record<string, ParamValue> }).params ?? {};
	params[name] = value;
	(held as { params?: Record<string, ParamValue> }).params = params;
	return true;
}

/** One board by id, through `hasOwn` — an id is untrusted input. */
function boardOf(project: Project, board: BoardId) {
	return Object.hasOwn(project.boards, board)
		? project.boards[board]
		: undefined;
}

/** One wire by id. */
function wireOf(
	project: Project,
	board: BoardId,
	wire: WireId,
): Wire | undefined {
	return boardOf(project, board)?.wires.find(
		(candidate) => candidate.id === wire,
	);
}
