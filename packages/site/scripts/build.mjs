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

/* The editor is staged when it has been built and skipped, loudly, when it has
 * not. Skipping rather than failing is deliberate: the site is the engine's
 * deploy and predates the editor, so an unbuilt editor must not be able to take
 * the shell offline. CI builds it before it gets here, so the skip is a local
 * convenience rather than a state the deploy is ever in — which is exactly why
 * it says so on stdout instead of passing silently. */
const editorBuilt = editorIsBuilt();

const staged = stageSite({
	artifacts: artifacts(),
	outDir: OUT,
	stem: manifest.cacheStem,
	assetDir: assetDir(),
	editorDir: editorBuilt ? editorDistDir : null,
});

process.stdout.write(
	`@kruddage/site: staged ${manifest.version} into ${OUT} ` +
		`(stem ${manifest.cacheStem})\n`
);
for (const name of staged) process.stdout.write(`  ${name}\n`);

if (!editorBuilt) {
	process.stdout.write(
		`\n  note: @kruddage/editor is not built, so /editor/ was not staged.\n` +
			`        ${EDITOR_BUILD_COMMAND}\n`
	);
}
