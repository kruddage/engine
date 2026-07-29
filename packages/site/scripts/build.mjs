// SPDX-License-Identifier: GPL-2.0-or-later
//
// Stages the deployable site into packages/site/dist.
//
// Note what this file does *not* do: it never looks at <repo>/build, never runs
// kruddmake, and never derives the cache-busting hash itself. It asks
// @kruddage/engine what was built and what the hash is. That is the whole point
// of the split — stage-site.sh ran `git rev-parse --short HEAD` and trusted that
// it matched the hash the Scheme had independently baked into the shell
// template. Two derivations that had to agree, with nothing checking that they
// did. Now there is one, and it comes from the artifact.

import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import {
	BUILD_COMMAND as EDITOR_BUILD_COMMAND,
	distDir as editorDistDir,
	isBuilt as editorIsBuilt,
} from "@kruddage/editor";
import { artifacts, assetDir, readManifest } from "@kruddage/engine";

import { stageSite } from "../src/stage.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = process.argv[2]
	? resolve(process.argv[2])
	: join(resolve(HERE, ".."), "dist");

const manifest = readManifest();

/* An unbuilt editor used to be a note on stdout: the site was the engine's
 * deploy, the editor was a skeleton at /editor/, and skipping it left the shell
 * serving at the root exactly as before. #953 moved the editor to the root, so
 * skipping it now stages a site whose entry document does not exist — a deploy
 * that 404s on the way in and nowhere else. It is an error, and it is the kind
 * worth being loud about, because the only way to hit it is to run the steps out
 * of order. */
if (!editorIsBuilt()) {
	process.stderr.write(
		`@kruddage/site: @kruddage/editor is not built, and it is the site root.\n` +
			`  ${EDITOR_BUILD_COMMAND}\n`
	);
	process.exit(1);
}

const staged = stageSite({
	artifacts: artifacts(),
	outDir: OUT,
	stem: manifest.cacheStem,
	assetDir: assetDir(),
	editorDir: editorDistDir,
});

process.stdout.write(
	`@kruddage/site: staged ${manifest.version} into ${OUT} ` +
		`(stem ${manifest.cacheStem})\n`
);
for (const name of staged) process.stdout.write(`  ${name}\n`);
