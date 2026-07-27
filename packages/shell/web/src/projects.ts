// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Which project the page opens with.
 *
 * **Tier: `shell`.** Which boards exist is `@krudd/board`'s; which one this
 * host boots is the host's, and it is a URL away rather than a constant
 * because a project is data. The page opens on tic-tac-toe — a board you can
 * play is a better first thing to meet than one you can only watch — and the
 * demo is still there under `?project=triangles`.
 *
 * That link is not a convenience. `render-test` screenshots whatever the page
 * boots and compares it against a checked-in reference of the drifting
 * triangles, so the demo has to stay reachable by name for the reference to go
 * on meaning what it meant. See `harness/render-test.ts`.
 *
 * Its own module rather than a few constants in `index.ts` so that it can be
 * tested: `index.ts` boots the engine as a side effect of being imported, and
 * a test that imported it would be a test that tried to open WebGL.
 */

import type { Project } from "@krudd/board";
import { TICTACTOE, TRIANGLES } from "@krudd/board";

/** The projects the page can boot, by the name `?project=` takes. */
export const PROJECTS: Readonly<Record<string, Project>> = {
	"tic-tac-toe": TICTACTOE,
	triangles: TRIANGLES,
};

/** What the page opens on when the URL does not say. */
export const DEFAULT_PROJECT = "tic-tac-toe";

/** The query parameter that names one of [`PROJECTS`]. */
export const PROJECT_PARAM = "project";

/**
 * The project a query string asks for, or the default.
 *
 * A name the page does not hold throws rather than quietly booting the
 * default: a link that says `?project=tictactoe` was written by somebody who
 * meant a particular board, and opening a different one silently would leave
 * them looking at the wrong game with no way to tell that is what happened.
 */
export function chosenProject(search: string): Project {
	const asked = new URLSearchParams(search).get(PROJECT_PARAM);
	const name = asked === null || asked === "" ? DEFAULT_PROJECT : asked;
	const held = Object.hasOwn(PROJECTS, name) ? PROJECTS[name] : undefined;
	if (held === undefined) {
		throw new Error(
			`there is no project named \`${name}\` — the page holds ` +
				`${Object.keys(PROJECTS).join(", ")}`,
		);
	}
	return held;
}
