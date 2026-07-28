// SPDX-License-Identifier: GPL-2.0-or-later
//
// Component-suite setup.
//
// Two jobs, both about isolation between tests: unmount React trees, and forget
// that a boot happened. The second matters because boot.ts guards against
// double-booting with module-scoped state — correct for a page, wrong for a
// suite where every test is a fresh page.

import { afterEach } from "vitest";
import { cleanup } from "@testing-library/react";

import { resetBootStateForTests } from "../src/engine/boot.js";

afterEach(() => {
	cleanup();
	resetBootStateForTests();
});
