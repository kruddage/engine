// SPDX-License-Identifier: GPL-2.0-or-later
//
// Removes this package's build output and test artifacts.

import { rmSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const PACKAGE = resolve(dirname(fileURLToPath(import.meta.url)), "..");

for (const name of ["dist", "playwright-report", "test-results"]) {
	rmSync(join(PACKAGE, name), { recursive: true, force: true });
}
