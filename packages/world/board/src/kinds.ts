// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * The node kinds the triangles project is made of.
 *
 * Every one of them is a pure function of (columns, params) — that is the
 * rule that keeps a later compile-to-TypeScript step a transform rather than
 * a redesign, and it is why a kind declares its ports and params rather than
 * reaching for the world it happens to be running in.
 *
 * The port types say the other rule out loud: what moves between nodes is
 * `column<vec3>`, a whole column, never one entity's `vec3`. `integrate`
 * takes the position and velocity columns and walks them once per frame.
 * `recycle` already exists in exactly this shape, hand-written, in
 * `packages/shell/web/src/index.ts` — it is a node nobody had drawn yet.
 *
 * A node never hardcodes which column it walks — it resolves a name from a
 * `column-name` param, once per phase, and asks the world for that column.
 * `position` and `velocity` are only the defaults; they are what makes the
 * triangles project keep running unchanged once the board declares them
 * itself. See `document.ts`'s `BoardColumn` for why the declaration lives on
 * the board rather than on whichever node happens to output the column.
 *
 * The defaults here are the constants the engine runs today, not
 * placeholders: changing `radius` here is changing where the triangles
 * spawn.
 */

import type { NodeKind, Registry } from "./document";
import { pickCell } from "./pick";
import { winnerOf } from "./win";

/** A whole column of positions or velocities, three floats per slot. */
const COLUMN: "column<vec3>" = "column<vec3>";

/** The param type a column-name param declares itself with. */
const COLUMN_NAME: "column-name" = "column-name";

/**
 * The kinds, by name.
 *
 * Names are lower-case and hyphenated; the title a node box shows is the
 * kind's, so the two never have to be kept in step by hand.
 */
export const KINDS: Registry = {
	start: {
		title: "Start",
		entry: "start",
		inputs: [],
		outputs: [],
		params: [],
	},

	"spawn-ring": {
		title: "Spawn Ring",
		inputs: [],
		outputs: [
			{ name: "position", type: COLUMN },
			{ name: "velocity", type: COLUMN },
		],
		params: [
			{ name: "count", type: "u32", default: 8, min: 0, max: 65536 },
			{ name: "radius", type: "f32", default: 0.4, min: 0 },
			{ name: "speed", type: "f32", default: 0.25, min: 0 },
			{ name: "position", type: COLUMN_NAME, default: "position" },
			{ name: "velocity", type: COLUMN_NAME, default: "velocity" },
		],
		run: (c) => {
			const count = c.number("count");
			const radius = c.number("radius");
			const speed = c.number("speed");
			// Slots first. Allocating is the one per-entity crossing left, and
			// it happens once when the board opens rather than once a frame, so
			// the rule the columns exist for is intact. A board that spawns
			// during play wants a batched allocate on the boundary; nothing does
			// yet.
			for (let slot = c.world.slotCount; slot < count; slot++) {
				c.world.spawn(0, 0, 0);
			}
			// Fetched after the spawns, never before: a spawn can move the
			// column, and a view taken first would be writing into memory that
			// is no longer it. Which column is this node's own business only in
			// name — `validate` has already checked the board declares it at a
			// matching kind.
			const position = c.world.column(c.text("position")) as Float32Array;
			const velocity = c.world.column(c.text("velocity")) as Float32Array;
			for (let i = 0; i < count; i++) {
				const angle = (i / count) * Math.PI * 2;
				position[i * 3] = Math.cos(angle) * radius;
				position[i * 3 + 1] = Math.sin(angle) * radius;
				position[i * 3 + 2] = 0;
				velocity[i * 3] = Math.cos(angle) * speed;
				velocity[i * 3 + 1] = Math.sin(angle) * speed;
				velocity[i * 3 + 2] = 0;
			}
		},
	},

	step: {
		title: "Step",
		entry: "step",
		inputs: [],
		outputs: [],
		params: [],
	},

	integrate: {
		title: "Integrate",
		inputs: [
			{ name: "position", type: COLUMN },
			{ name: "velocity", type: COLUMN },
		],
		outputs: [{ name: "position", type: COLUMN }],
		params: [
			{ name: "position", type: COLUMN_NAME, default: "position" },
			{ name: "velocity", type: COLUMN_NAME, default: "velocity" },
		],
		run: (c) => {
			const position = c.world.column(c.text("position")) as Float32Array;
			const velocity = c.world.column(c.text("velocity")) as Float32Array;
			// The whole column in one walk, tombstones included. A tombstoned
			// slot has a zero velocity and so integrates to itself, which is
			// cheaper than asking the engine which slots are live — and asking
			// per slot is the crossing this is all here to avoid.
			for (let i = 0; i < position.length; i += 3) {
				position[i] = (position[i] as number) + (velocity[i] as number) * c.dt;
				position[i + 1] =
					(position[i + 1] as number) + (velocity[i + 1] as number) * c.dt;
				position[i + 2] =
					(position[i + 2] as number) + (velocity[i + 2] as number) * c.dt;
			}
		},
	},

	recycle: {
		title: "Recycle",
		inputs: [{ name: "position", type: COLUMN }],
		outputs: [{ name: "position", type: COLUMN }],
		params: [
			// Comfortably outside the camera's box, so a recycle happens off
			// screen rather than as a visible jump.
			{ name: "limit", type: "f32", default: 3.2, min: 0 },
			{ name: "radius", type: "f32", default: 0.4, min: 0 },
			{ name: "position", type: COLUMN_NAME, default: "position" },
		],
		run: (c) => {
			const limit = c.number("limit");
			const radius = c.number("radius");
			const position = c.world.column(c.text("position")) as Float32Array;
			for (let i = 0; i < position.length; i += 3) {
				const x = position[i] as number;
				const y = position[i + 1] as number;
				const distance = Math.hypot(x, y);
				if (distance <= limit) {
					continue;
				}
				// Back onto the circle along the same ray, so an entity sets off
				// again in the direction it left by.
				const scale = radius / distance;
				position[i] = x * scale;
				position[i + 1] = y * scale;
			}
		},
	},

	paint: {
		title: "Paint",
		entry: "paint",
		inputs: [],
		outputs: [],
		params: [],
	},

	"draw-entities": {
		title: "Draw Entities",
		inputs: [{ name: "position", type: COLUMN }],
		outputs: [],
		params: [
			// One mesh so far, so `mesh` reports rather than decides. `scale`
			// decides: it reaches the transform of every draw, because a control
			// that looks live and changes nothing teaches the wrong thing about
			// what a board is.
			{ name: "mesh", type: "mesh", default: "triangle" },
			{ name: "scale", type: "f32", default: 0.35, min: 0 },
			// No column-name param here, unlike every other kind that walks a
			// column. `render()` builds its draw list inside the engine from the
			// engine's own position column, so a name offered here could only be
			// read and ignored — the settings sheet would show a control that
			// edits the document and changes nothing on screen. The input port
			// above already records which column this depends on. When the
			// renderer can take a column by name, the param arrives with the
			// behaviour rather than ahead of it.
		],
		run: (c) => {
			// Two crossings for the whole frame, whatever the draw count. The
			// draw list is built inside the engine from the position column —
			// the same column every node above has been walking.
			c.world.setScale(c.number("scale"));
			c.world.render();
		},
	},

	// One level down: the frame itself. Viewable rather than authorable —
	// these describe what the renderer already does, so that opening
	// `Draw Entities` shows clear → camera → draw → present instead of a
	// dead end. Authoring arbitrary render pipelines is a different problem
	// with a different audience.

	"begin-frame": {
		title: "Begin Frame",
		entry: "start",
		inputs: [],
		outputs: [{ name: "frame", type: "frame" }],
		params: [{ name: "surface", type: "surface", default: "canvas" }],
	},

	clear: {
		title: "Clear",
		inputs: [{ name: "frame", type: "frame" }],
		outputs: [{ name: "frame", type: "frame" }],
		// Black, which is what `Frame::new` clears to. The first thing the eye
		// sees is a setting rather than a constant.
		params: [{ name: "colour", type: "color", default: "#000000" }],
	},

	camera: {
		title: "Camera",
		inputs: [],
		outputs: [{ name: "viewProjection", type: "mat4" }],
		// Half the height of the visible world, in world units — VIEW_EXTENT in
		// crates/shell/web. The box is widened by the canvas aspect rather than
		// squashed, so a wider window shows more world.
		params: [{ name: "extent", type: "f32", default: 2, min: 0 }],
	},

	draw: {
		title: "Draw",
		inputs: [
			{ name: "position", type: COLUMN },
			{ name: "viewProjection", type: "mat4" },
		],
		outputs: [],
		params: [
			{ name: "pipeline", type: "mesh", default: "triangle" },
			{ name: "vertices", type: "u32", default: 3, min: 1 },
		],
	},

	present: {
		title: "Present",
		inputs: [{ name: "frame", type: "frame" }],
		outputs: [],
		params: [{ name: "to", type: "surface", default: "surface" }],
	},

	// The first input of any kind to reach the graph. See `RunContext.pointer`
	// in `./world` for what it carries and where the edge — the debounce that
	// makes a held press place once, not every frame it stays down — is found.

	pointer: {
		title: "Pointer",
		inputs: [],
		outputs: [
			{ name: "x", type: "f32" },
			{ name: "y", type: "f32" },
			{ name: "pressed", type: "u32" },
		],
		params: [],
		run: () => {
			// Nothing to compute. `x`, `y` and `pressed` are already resolved
			// before any node runs — `c.pointer`, on every `RunContext` — so a
			// kind downstream reads them straight off the context, the same way
			// `integrate` reads `c.world.positions()` rather than a wire this
			// interpreter evaluates. This kind exists so a document has
			// something concrete to wire from: a name a wire leaves by, and a
			// type the validator can check the far end against.
		},
	},

	// Grid picking (#866 PR-4). Turns a pointer position into a tic-tac-toe
	// cell, but does nothing with the answer yet — see `pick-grid` below for
	// why, and `./pick` for the arithmetic itself.

	"pick-grid": {
		title: "Pick Grid",
		inputs: [],
		outputs: [
			{ name: "cell", type: "u32" },
			{ name: "hit", type: "u32" },
		],
		params: [
			// The engine's own default box is `VIEW_EXTENT * 2` on a side; the
			// grid does not have to fill it, so this is its own number rather
			// than a read of that constant.
			{ name: "size", type: "f32", default: 2.0, min: 0 },
		],
		run: () => {
			// Nothing to compute here, and — unlike `pointer` — not because the
			// answer is already sitting on `RunContext`. It is because nothing
			// reads it yet: `place-mark` is PR-6's business, and this PR's scope
			// stops at declaring the shape of a pick and proving the arithmetic
			// correct in isolation. `./pick`'s `pickCell` is that arithmetic —
			// pure, exported, and unit-tested there — ready for PR-6 to call
			// once there is a `mark` column and a cell to write it into.
			//
			// This mirrors `pointer` more than it first looks: `pointer`'s own
			// `run` does not compute `x`/`y`/`pressed` either — that happens in
			// `packages/shell/web/src/pointer.ts`'s `PointerTrack`, entirely
			// outside this kind. In both cases the kind exists to give a
			// document a name to wire from and a type the validator can check,
			// not to be where the arithmetic runs. What's different here is
			// that nothing has wired the result anywhere yet — no `RunContext`
			// field, no column — so there is nothing for this `run` to hand
			// off to. Writing a column from here would be inventing the
			// wire-value evaluator this board deliberately does not have.
		},
	},

	// Tic-tac-toe rules (#866 PR-6). The issue's criteria list three kinds —
	// `spawn-grid`, `place-mark`, `detect-win` — and only two are built. Win
	// detection is `./win`'s `winnerOf`: a plain exported function
	// `place-mark` calls, not a node of its own.
	//
	// The reason, in one place rather than repeated at each call site: turn
	// alternation depends on whether the move that just landed was terminal —
	// a win or a draw does not flip the turn, anything else does — and the
	// only moment that fact is known is right after the mark is placed. The
	// pre-rewrite `ttt-place-move` (`krudd/engine/game/tictactoe/rules.scm`)
	// checked the winner and flipped the turn in the same function for
	// exactly that reason. A `place-mark` that only placed and a `detect-win`
	// that only detected would need the first to tell the second "a mark
	// landed this frame, here is where" — that is state passed *between*
	// nodes, which invariant 3 (every node a pure function of its own columns
	// and params) forbids outright. One node doing the whole move needs no
	// such channel: it reads the columns, does the placement and the
	// resolution, and writes the columns back, all in the one call.

	"spawn-grid": {
		title: "Spawn Grid",
		inputs: [],
		outputs: [{ name: "position", type: COLUMN }],
		params: [
			// The grid's full width/height in world units — the same
			// convention `pick-grid` uses, and deliberately the same
			// default, so a board that leaves both at their defaults keeps
			// the cells it draws and the cells it picks lined up.
			{ name: "size", type: "f32", default: 2.0, min: 0 },
			{ name: "position", type: COLUMN_NAME, default: "position" },
		],
		run: (c) => {
			const size = c.number("size");
			// Slots first, and only as many as are missing — the same
			// idempotency `spawn-ring`'s own loop gets for free from
			// starting at `slotCount` rather than at 0. A second run against
			// a world that already holds the nine cells spawns nothing more.
			for (let slot = c.world.slotCount; slot < 9; slot++) {
				c.world.spawn(0, 0, 0);
			}
			const position = c.world.column(c.text("position")) as Float32Array;
			const half = size / 2;
			const step = size / 3;
			// Row 0 at the top, cell 0 top-left, index = row * 3 + col —
			// exactly `pick.ts`'s own convention, and exactly what
			// `pick.test.ts`'s `centre` helper computes. A pick that
			// disagreed with where the cells were actually drawn is the bug
			// this line-for-line match exists to avoid.
			for (let row = 0; row < 3; row++) {
				for (let col = 0; col < 3; col++) {
					const i = row * 3 + col;
					position[i * 3] = -half + (col + 0.5) * step;
					position[i * 3 + 1] = half - (row + 0.5) * step;
					position[i * 3 + 2] = 0;
				}
			}
		},
	},

	"place-mark": {
		title: "Place Mark",
		inputs: [],
		outputs: [],
		params: [
			// Kept equal to `spawn-grid`'s own default for the same reason
			// `pick-grid`'s and `spawn-grid`'s are kept equal: a pick has to
			// agree with where the cells it picks among were actually drawn.
			{ name: "size", type: "f32", default: 2.0, min: 0 },
			{ name: "mark", type: COLUMN_NAME, default: "mark" },
			{ name: "won", type: COLUMN_NAME, default: "won" },
			{ name: "turn", type: COLUMN_NAME, default: "turn" },
			{ name: "over", type: COLUMN_NAME, default: "over" },
		],
		run: (c) => {
			const size = c.number("size");
			// Unprojected through the world's own camera arithmetic, not a
			// second copy of it — see `./pick`'s module docs for why
			// `pickCell` never does this itself.
			const worldX = c.world.worldXFromScreen(c.pointer.x);
			const worldY = c.world.worldYFromScreen(c.pointer.y);
			const pick = pickCell(worldX, worldY, size, c.pointer.pressed);
			// 1. No press this frame, or the pick missed the grid: nothing
			// to do. `pickCell` already folds both conditions into `hit`, so
			// one check covers both halves of the rule.
			if (pick.hit !== 1) {
				return;
			}

			const mark = c.world.column(c.text("mark")) as Uint32Array;
			const won = c.world.column(c.text("won")) as Uint32Array;
			const turn = c.world.column(c.text("turn")) as Uint32Array;
			const over = c.world.column(c.text("over")) as Uint32Array;

			// `turn` and `over` are scalars at slot 0 of a column sized to
			// capacity — a wart the module docs above `KINDS` accept
			// deliberately, in exchange for every node staying a pure
			// function of (columns, params). Sized to capacity means
			// zero-length before anything has been spawned, and every
			// column grows together, so this one check also stands in for
			// `mark` and `won` being empty. A tap that lands before
			// `spawn-grid` has run must do nothing, not throw.
			if (turn.length === 0 || over.length === 0) {
				return;
			}

			// 2. The round is already over: any tap restarts it, clearing
			// the board but not counting as a placement of its own.
			if ((over[0] as number) !== 0) {
				mark.fill(0);
				won.fill(0);
				turn[0] = 1;
				over[0] = 0;
				return;
			}

			// 3. The cell is already taken: a complete no-op — no mark, no
			// turn change.
			const cell = pick.cell;
			if ((mark[cell] as number) !== 0) {
				return;
			}

			// 4. Place. `turn` reads zero-filled the very first time this
			// runs against a freshly spawned board — 0 is not one of the two
			// legal values (1 or 2), because nothing else ever initialises
			// it: there is no third "load" node the way the pre-rewrite
			// Scheme had a module-level `(define *ttt-turn* 1)` that ran once
			// when the image loaded. So an untouched column reads as X to
			// move here, which is the same default the old global carried,
			// rather than as a third, illegal mark value. Every write after
			// this one puts an explicit 1 or 2 back, by a restart or by the
			// flip below, so the fallback only ever matters for this first
			// placement of a game that has never been played or restarted.
			const player: 1 | 2 = (turn[0] as number) === 2 ? 2 : 1;
			mark[cell] = player;

			// 5. Resolve. Win is tested before draw — the move that
			// completes a line and fills the board at once is a win, not a
			// draw — and neither flips the turn; only an ordinary move does.
			const result = winnerOf(mark);
			if (result.winner !== 0) {
				over[0] = result.winner;
				if (result.line !== undefined) {
					for (const i of result.line) {
						won[i] = 1;
					}
				}
				return;
			}
			let full = true;
			for (let i = 0; i < 9; i++) {
				if ((mark[i] as number) === 0) {
					full = false;
					break;
				}
			}
			if (full) {
				over[0] = 3;
				return;
			}
			turn[0] = player === 1 ? 2 : 1;
		},
	},
};

/**
 * One kind by name, or `undefined` if the registry does not hold it.
 *
 * The `hasOwn` is the whole function. A document names its kinds as strings,
 * and a document is untrusted input: indexing a plain object with
 * `"constructor"` or `"toString"` walks the prototype chain and hands back
 * something that is not a kind but is not `undefined` either. Every lookup
 * keyed by a name a document chose goes through here or through
 * `Object.hasOwn` for that reason.
 */
export function kindOf(kinds: Registry, name: string): NodeKind | undefined {
	return Object.hasOwn(kinds, name) ? kinds[name] : undefined;
}
