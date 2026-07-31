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

import { artifacts, assetDir, readManifest } from "@kruddage/engine";

import { stageSite } from "../src/stage.mjs";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = process.argv[2]
	? resolve(process.argv[2])
	: join(resolve(HERE, ".."), "dist");

const manifest = readManifest();

const staged = stageSite({
	artifacts: artifacts(),
	outDir: OUT,
	stem: manifest.cacheStem,
	assetDir: assetDir(),
});

process.stdout.write(
	`@kruddage/site: staged ${manifest.version} into ${OUT} ` +
		`(stem ${manifest.cacheStem})\n`
);
for (const name of staged) process.stdout.write(`  ${name}\n`);
