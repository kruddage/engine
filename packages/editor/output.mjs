// SPDX-License-Identifier: GPL-2.0-or-later
//
// @kruddage/editor's Node-facing surface: where its build output is.
//
// The editor is an application, not a library, so this is the whole of what it
// offers another package — one directory and the question of whether anything
// is in it. @kruddage/site needs exactly that to stage the editor as the site
// root (#953; it was staged beside the shell at editor/ before), and the
// workspace boundary check requires it to ask through the package name rather
// than reaching in by relative path. That rule is why this file exists rather
// than @kruddage/site joining "../editor/dist" together.
//
// Nothing in here is bundled, imported by the app, or reachable from a browser.
// It is the mirror of @kruddage/engine's much larger surface, kept deliberately
// small because there is genuinely nothing else to hand out honestly.

import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));

/** Absolute path to this package's production build. */
export const distDir = resolve(HERE, "dist");

/** The command that fills distDir. */
export const BUILD_COMMAND = "pnpm --filter @kruddage/editor run build";

/**
 * Whether this package has been built.
 *
 * Keyed on the entry document rather than on the directory: `vite build` writes
 * index.html last-ish and an empty or half-written dist/ is exactly the state
 * this is asked about. A directory that exists but holds no page is not a
 * build.
 */
export function isBuilt() {
	return existsSync(join(distDir, "index.html"));
}
