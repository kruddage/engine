// SPDX-License-Identifier: GPL-2.0-or-later
//
// Removes this package's staged output. kruddmake's own build directory is left
// alone: it is incremental by design and rebuilding it from scratch costs
// minutes, so `pnpm clean` drops the copy, not the cache. Remove <repo>/build
// by hand for a genuinely cold build.

import { rmSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const DIST = join(resolve(dirname(fileURLToPath(import.meta.url)), ".."), "dist");

rmSync(DIST, { recursive: true, force: true });
process.stdout.write(`@kruddage/engine: removed ${DIST}\n`);
