// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * A project, as a file.
 *
 * ## The payload is the document, and nothing around it
 *
 * A project file is what `serializeProject` writes, saved under a name ending
 * `.krudd`, with a media type of `application/json`. There is no container:
 * no zip, no manifest, no directory of parts. While everything is a board and
 * there are no binary assets, a container would hold exactly one entry — and
 * it would cost a dependency, a corruption mode, and the stable bytes that
 * make a project diff. Meshes, textures and sounds are when a container earns
 * its keep, and the migration is additive when they arrive: a file beginning
 * `{` is a document, and a file beginning `PK` is a container whose first
 * entry is this exact document.
 *
 * ## What is not here
 *
 * Reading and writing, which are `parseProject` and `serializeProject` — a
 * file is those bytes and the name they are saved under, and this module is
 * only the second half. Nothing here touches a file system or a browser
 * either: `world` describes what a project *is*, and the shell it is running
 * in is what knows how files are chosen and handed back on that platform.
 */

/** What a project file is called. */
export const PROJECT_EXTENSION = ".krudd";

/**
 * What a project file holds.
 *
 * `application/json` rather than a bespoke type, because that is what it is.
 * A type nothing else recognises would mean a project that no tool can
 * pretty-print, diff or syntax-highlight without being taught to first.
 */
export const PROJECT_MEDIA_TYPE = "application/json";

/** What a project is called when nothing has named it. */
export const DEFAULT_PROJECT_NAME = "project";

/**
 * Control characters and the punctuation file systems reserve.
 *
 * A board's title is somebody's text and may hold anything at all — a slash
 * in a saved file name is a path, and a path is how a save ends up somewhere
 * nobody asked for.
 */
// biome-ignore lint/suspicious/noControlCharactersInRegex: a file name may not carry one, which is the point of the class
const RESERVED = /[\u0000-\u001F\u007F<>:"/\\|?*]+/g;

/**
 * The file name a project saves under.
 *
 * Always ends in exactly one [`PROJECT_EXTENSION`], whether or not the name
 * it was given did: `triangles`, `triangles.krudd` and `triangles.krudd.krudd`
 * all save as `triangles.krudd`. A name that is empty once the reserved
 * characters are out — or that was nothing but a leading dot, which is a
 * hidden file rather than a name — falls back to [`DEFAULT_PROJECT_NAME`],
 * because a save the user cannot find afterwards is a save that did not
 * happen.
 */
export function projectFileName(name?: string): string {
	let stem = (name ?? "").replace(RESERVED, " ").replace(/\s+/g, " ").trim();
	// Repeatedly, so that a title which already carries the extension twice
	// does not save with one of them still on it.
	while (stem.toLowerCase().endsWith(PROJECT_EXTENSION)) {
		stem = stem.slice(0, -PROJECT_EXTENSION.length).trim();
	}
	stem = stem.replace(/^\.+/, "").trim();
	return `${stem === "" ? DEFAULT_PROJECT_NAME : stem}${PROJECT_EXTENSION}`;
}
