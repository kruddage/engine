// SPDX-License-Identifier: GPL-2.0-or-later
//
// The editor's build.
//
// `base` is relative on purpose. The editor is deployed beside the existing
// shell at its own route rather than replacing it (see README.md, "Where the
// page comes from"), and a relative base is what lets the same output work at
// /editor/ on the deployed site and at / under `vite dev` without a second
// build. It is also what makes a preview deploy — which lands under a PR-number
// prefix — work without knowing its own URL.

import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

import { engineArtifacts } from "./build/engine-artifacts.mjs";

export default defineConfig({
	base: "./",
	plugins: [react(), engineArtifacts()],
	build: {
		outDir: "dist",
		emptyOutDir: true,
		/* The engine's own artifacts are copied in by the plugin after the
		 * bundle closes. Nothing here should try to fingerprint or rewrite
		 * them: index.wasm is resolved at runtime through the loader's
		 * locateFile hook, and a bundler that renamed it would break the one
		 * reference that finds it. */
		sourcemap: true,
	},
	server: {
		/* No proxy, no rewrite: the engine's artifacts are served by our own
		 * middleware from the package that owns them. */
		fs: { strict: true },
	},
});
