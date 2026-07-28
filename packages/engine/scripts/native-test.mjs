// SPDX-License-Identifier: GPL-2.0-or-later
//
// `pnpm --filter @kruddage/engine test:native` — the whole native suite.
//
// A two-line wrapper on purpose. The suite itself is kruddmake's
// run-tests.sh: POSIX shell, no node in its path, and the thing CI's sanitizer
// and coverage jobs invoke directly. This exists so the engine package reaches
// it the same way it reaches the build — through the declared dependency on
// @kruddage/kruddmake rather than a relative path into krudd/ (#920) — and so
// there is one place that knows kruddmake's layout.
//
// If you have a compiler and no node, run it directly:
//
//   sh krudd/kruddmake/run-tests.sh

import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

import { KRUDDMAKE_TESTS, runKruddmake } from "./kruddmake.mjs";

const REPO = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");

process.exit(runKruddmake(KRUDDMAKE_TESTS, [], { cwd: REPO }));
