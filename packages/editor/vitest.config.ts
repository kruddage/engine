// SPDX-License-Identifier: GPL-2.0-or-later
//
// The component suite (#944, Q6).
//
// Vitest is the default and it is where a test goes unless it would lie here.
// The browser suite is Playwright, and the line between them is drawn in
// README.md rather than left to taste — a browser suite that grows to cover
// what jsdom could have answered is how a fast suite becomes one nobody runs.
//
// This config does not load vite.config.ts. That file installs the engine
// plugin, which reads @kruddage/engine's build output; the component suite must
// not depend on whether the engine has been built, so the virtual module it
// provides is stubbed in test/engine-stub.ts instead. The stub is typed against
// the same EngineInfo the plugin emits, so a drift between them is a type
// error rather than a green suite testing nothing.

import { defineConfig } from "vitest/config";
import react from "@vitejs/plugin-react";

export default defineConfig({
	plugins: [react()],
	resolve: {
		alias: [{ find: "virtual:krudd-engine", replacement: "/test/engine-stub.ts" }],
	},
	test: {
		/* jsdom is the default because most of this package is UI. The build
		 * plugin's own suite opts back out with a `@vitest-environment node`
		 * docblock — it reads the filesystem, and a DOM would be scenery. */
		environment: "jsdom",
		globals: false,
		setupFiles: ["./test/setup.ts"],
		include: ["test/**/*.test.ts", "test/**/*.test.tsx", "build/**/*.test.ts"],
	},
});
