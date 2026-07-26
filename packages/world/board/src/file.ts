// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * A project, as a file.
 *
 * ## The payload is the document, and nothing around it
 *
 * A project file is what `serializeProject` writes, saved under a name ending
 * `.json`, with a media type of `application/json`. There is no container:
 * no zip, no manifest, no directory of parts. While everything is a board and
 * there are no binary assets, a container would hold exactly one entry — and
 * it would cost a dependency, a corruption mode, and the stable bytes that
 * make a project diff.
 *
 * ## And the name says so too
 *
 * The extension is `.json` rather than a bespoke one because the bytes are
 * JSON and nothing else, and a name that says otherwise buys nothing. A
 * bespoke extension is a promise that the file needs krudd to be worth
 * opening; this one does not. Every editor already highlights it, every diff
 * already reads it, every review already renders it, and none of them had to
 * be taught. Claiming an extension is how a format stops being those things.
 *
 * Meshes, textures and sounds are the case that could change this, and the
 * migration stays additive when they arrive: assets encode into the document
 * itself or are referenced by URL from it, and either way the file is still
 * JSON. A container is only forced if some asset must travel as opaque bytes
 * in the same file — and that is a decision to make against a real asset,
 * not in advance of one.
 *
 * ## Opening does not care what the file is called
 *
 * `readProjectFile` checks the bytes rather than the name, so a project saved
 * under the older `.krudd` extension still opens, and so does one somebody
 * renamed. Nothing here is a compatibility shim — it is the same rule that
 * was always in force, and it is why changing the extension takes no
 * migration.
 *
 * ## What is not here
 *
 * Reading and writing, which are `parseProject` and `serializeProject` — a
 * file is those bytes and the name they are saved under, and this module is
 * only the second half. Nothing here touches a file system or a browser
 * either: `world` describes what a project *is*, and the shell it is running
 * in is what knows how files are chosen and handed back on that platform.
 */

/**
 * What a project file is called.
 *
 * `.json`, because that is what the bytes are. See the note above on why this
 * is not a bespoke extension.
 */
export const PROJECT_EXTENSION = ".json";

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
 * it was given did: `triangles`, `triangles.json` and `triangles.json.json`
 * all save as `triangles.json`. A name that is empty once the reserved
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
