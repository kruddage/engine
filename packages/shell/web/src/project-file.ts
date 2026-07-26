// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * Saving a project to a file, and loading one back.
 *
 * **Tier: `shell`.** What a project file *is* — JSON, named `.krudd`, with
 * nothing around it — belongs to `@krudd/board` and is the same wherever a
 * project is opened. How a file gets chosen, handed over and written back is
 * the host's, and on this host that means an anchor with a `download`
 * attribute and an `<input type="file">`. A `world`-tier package that knew
 * either of those would be a package that only runs on a page.
 *
 * ## A failed load leaves the board alone
 *
 * Everything that can refuse happens before anything is touched: the bytes
 * are parsed and validated here, and the caller builds the runner — which
 * refuses a kind that cannot run — before it clears the world. So a file that
 * does not hold together leaves the board on screen exactly as it was, with
 * the reason beside it. "Rather than booting into a broken board" is as much
 * about what is still running afterwards as about the refusal itself.
 */

import type { Project } from "@krudd/board";
import {
	BoardError,
	describe,
	PROJECT_EXTENSION,
	PROJECT_MEDIA_TYPE,
	parseProject,
	projectFileName,
	serializeProject,
} from "@krudd/board";

/** What a file picker should offer, given what a project file is. */
export const PROJECT_ACCEPT = `${PROJECT_EXTENSION},${PROJECT_MEDIA_TYPE}`;

/**
 * Hands a project to the browser as a download. Returns the name used.
 *
 * The name comes from the board the project opens on, so a saved file says
 * what it holds rather than what the page happened to call it.
 */
export function saveProject(project: Project, name?: string): string {
	const fileName = projectFileName(name ?? project.boards[project.root]?.title);
	const blob = new Blob([serializeProject(project)], {
		type: PROJECT_MEDIA_TYPE,
	});
	const url = URL.createObjectURL(blob);
	const link = document.createElement("a");
	link.href = url;
	link.download = fileName;
	link.click();
	// Revoked on a later turn, not this one: the download reads the URL after
	// the click returns, and revoking it immediately cancels the save in some
	// browsers — a save that silently did not happen, which is the failure this
	// whole path exists to not have.
	setTimeout(() => {
		URL.revokeObjectURL(url);
	}, 0);
	return fileName;
}

/**
 * Reads a project out of a file somebody picked.
 *
 * Throws a `BoardError` naming what is wrong — the version it got, the JSON
 * it could not parse, the node or wire that does not hold together — rather
 * than returning something half-read.
 *
 * The extension is not checked. A file the user chose is a file the user
 * meant, and refusing `triangles.json` for its name while the bytes are a
 * perfectly good project would be the page being fussy about the one thing it
 * can already tell for certain by reading it.
 */
export async function readProjectFile(file: File): Promise<Project> {
	return parseProject(await file.text());
}

/**
 * What to tell somebody whose file did not load.
 *
 * It names the file and then says what is still running, because the load
 * changed nothing — and a failure that leaves you guessing whether the board
 * you are looking at is the one you had is worse than the failure.
 */
export function loadFailure(fileName: string, error: unknown): string {
	return `${fileName} did not load:\n${reason(error)}\nThe board on screen is unchanged.`;
}

/**
 * What to tell somebody whose save did not happen.
 *
 * Said at all, which is the point: a download the browser declined leaves no
 * trace anywhere the user is looking, so a save that quietly did nothing is
 * indistinguishable from one that worked until the file is looked for.
 */
export function saveFailure(error: unknown): string {
	return `the project did not save:\n${reason(error)}`;
}

/**
 * Why something failed, as text.
 *
 * Every problem in a `BoardError`, not the first: a document with three things
 * wrong is a document somebody has to fix three times if they are only told
 * about one of them.
 */
function reason(error: unknown): string {
	if (error instanceof BoardError) {
		return error.problems.map(describe).join("\n");
	}
	return error instanceof Error ? error.message : String(error);
}
