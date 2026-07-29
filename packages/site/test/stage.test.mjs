// SPDX-License-Identifier: GPL-2.0-or-later
//
// Staging is checked against a synthetic build tree rather than a real one: the
// WASM link needs emsdk, and none of what staging decides depends on the bytes
// being genuine. The one place the real artifact matters — that index.html
// carries a locateFile stem at all — is checked by the engine package against
// the shell template, and by its build script against the built HTML.

import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";

import { ENGINE_ARTIFACTS } from "@kruddage/engine";

import { ENGINE_PREFIX, planSite, stageSite } from "../src/stage.mjs";

const STEM = "abc1234";

/* An index.html shaped like the real one: a loader tag emcc substituted in, a
 * locateFile hook carrying the stem, and the error overlay's literal mention of
 * index.wasm that must survive staging untouched. */
const INDEX_HTML = `<!doctype html>
<link rel="manifest" href="manifest.webmanifest">
<span class="error-source">index.wasm</span>
<script>
var Module = {
	locateFile: function (path, prefix) {
		if (path.endsWith('.wasm'))
			return prefix + path.replace(/\\.wasm$/, '.${STEM}.wasm');
		return prefix + path;
	}
};
</script>
<script async type="text/javascript" src="index.js"></script>
`;

function fakeBuild() {
	const dir = mkdtempSync(join(tmpdir(), "krudd-site-"));

	const artifacts = ENGINE_ARTIFACTS.map((artifact) => {
		const path = join(dir, artifact.name);
		writeFileSync(
			path,
			artifact.name === "index.html" ? INDEX_HTML : `bytes of ${artifact.name}`
		);
		return { ...artifact, path };
	});

	return { dir, artifacts };
}

test("renames only the artifacts declared cache-busting", () => {
	const { artifacts } = fakeBuild();
	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));

	stageSite({ artifacts, outDir: out, stem: STEM });

	assert.deepEqual(readdirSync(join(out, ENGINE_PREFIX)).sort(), [
		"icon-192.png",
		"icon-512.png",
		"index.abc1234.js",
		"index.abc1234.wasm",
		"index.html",
		"manifest.webmanifest",
		"sw.js",
	]);
});

test("rewrites the loader reference in the entry document", () => {
	const { artifacts } = fakeBuild();
	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));

	stageSite({ artifacts, outDir: out, stem: STEM });
	const html = readFileSync(join(out, ENGINE_PREFIX, "index.html"), "utf8");

	assert.match(html, /src="index\.abc1234\.js"/);
	assert.doesNotMatch(html, /src="index\.js"/);
});

test("leaves the error overlay's mention of index.wasm alone", () => {
	/* The regression this exists for: index.wasm is renamed on disk, so a
	 * staging step that rewrote "every renamed artifact" would rewrite this
	 * display text too and show the user a filename that never existed. The
	 * real URL is produced by locateFile at runtime, not by this string. */
	const { artifacts } = fakeBuild();
	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));

	stageSite({ artifacts, outDir: out, stem: STEM });
	const html = readFileSync(join(out, ENGINE_PREFIX, "index.html"), "utf8");

	assert.match(html, /<span class="error-source">index\.wasm<\/span>/);
});

test("the staged wasm name is the one locateFile will request", () => {
	/* The end-to-end invariant. locateFile turns a request for "index.wasm"
	 * into "index.<stem>.wasm"; staging must have written exactly that. */
	const { artifacts } = fakeBuild();
	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));

	stageSite({ artifacts, outDir: out, stem: STEM });

	const html = readFileSync(join(out, ENGINE_PREFIX, "index.html"), "utf8");
	const hook = html.match(/'\.([^.']+)\.wasm'/);
	assert.ok(hook, "staged HTML kept its locateFile hook");

	const requested = `index.${hook[1]}.wasm`;
	assert.ok(
		readdirSync(join(out, ENGINE_PREFIX)).includes(requested),
		`locateFile requests ${requested}, staged: ${readdirSync(join(out, ENGINE_PREFIX)).join(", ")}`
	);
});

test("the PWA files keep their literal names", () => {
	/* sw.js is registered by name and manifest.webmanifest is linked by name;
	 * renaming either breaks the reference. */
	const { artifacts } = fakeBuild();
	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));

	stageSite({ artifacts, outDir: out, stem: STEM });
	const staged = readdirSync(join(out, ENGINE_PREFIX));

	for (const name of ["sw.js", "manifest.webmanifest", "icon-192.png"]) {
		assert.ok(staged.includes(name), `${name} kept its name`);
	}
});

test("copies the runtime asset directory when there is one", () => {
	const { dir, artifacts } = fakeBuild();
	const assets = join(dir, "assets");
	mkdirSync(assets);
	writeFileSync(join(assets, "chess.mesh"), "mesh");

	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));
	stageSite({ artifacts, outDir: out, stem: STEM, assetDir: assets });

	assert.equal(readFileSync(join(out, ENGINE_PREFIX, "assets", "chess.mesh"), "utf8"), "mesh");
});

test("fails loudly when the entry document lost its loader reference", () => {
	/* stage-site.sh used sed, which silently succeeds on no match — it would
	 * have staged an index.html pointing at a file nothing wrote. */
	const { dir, artifacts } = fakeBuild();
	writeFileSync(join(dir, "index.html"), "<!doctype html><p>no script tag</p>");

	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));
	assert.throws(
		() => stageSite({ artifacts, outDir: out, stem: STEM }),
		/expected a reference to index\.js/
	);
});

test("refuses to stage without a cache-busting stem", () => {
	const { artifacts } = fakeBuild();
	assert.throws(() => planSite(artifacts, ""), /stem is required/);
});

test("refuses a build with no single entry document", () => {
	const { artifacts } = fakeBuild();
	const withoutEntry = artifacts.filter((a) => a.role !== "entry");
	assert.throws(() => planSite(withoutEntry, STEM), /exactly one entry/);
});

test("staging is idempotent", () => {
	const { artifacts } = fakeBuild();
	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));

	const first = stageSite({ artifacts, outDir: out, stem: STEM });
	const second = stageSite({ artifacts, outDir: out, stem: STEM });

	assert.deepEqual(first, second);
});

/* The editor and the engine are two writers into one output directory. Until
 * #953 the engine held the root and the editor sat at `editor/`; now it is the
 * other way round. These check the arrangement itself, and that neither writer
 * disturbs the other — which is the whole risk in staging two builds together.
 *
 * The bug the last of them exists for is the one the user hit: a site that
 * staged and deployed cleanly while the root still served the old shell, so
 * eight merged PRs' worth of editor was live and unreachable. */
function fakeEditor() {
	const editor = mkdtempSync(join(tmpdir(), "krudd-editor-"));
	mkdirSync(join(editor, "assets"), { recursive: true });
	writeFileSync(join(editor, "index.html"), "<!doctype html>editor\n");
	writeFileSync(join(editor, "assets", "index-abc.js"), "/* bundle */\n");
	return editor;
}

test("stages the editor at the site root", () => {
	const { artifacts } = fakeBuild();
	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));

	const staged = stageSite({
		artifacts,
		outDir: out,
		stem: STEM,
		editorDir: fakeEditor(),
	});

	assert.ok(staged.includes("./"));
	assert.equal(
		readFileSync(join(out, "index.html"), "utf8"),
		"<!doctype html>editor\n"
	);
	/* The bundler's own hashed output comes across whole — the engine's
	 * artifact whitelist does not apply to it and must not be made to. */
	assert.ok(readdirSync(join(out, "assets")).includes("index-abc.js"));
});

test("the engine's page is the one under the engine's route", () => {
	const { artifacts } = fakeBuild();
	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));

	stageSite({ artifacts, outDir: out, stem: STEM, editorDir: fakeEditor() });

	/* Two documents both called index.html, and each has to be the right one.
	 * A regression that reversed the two would leave the site serving the game
	 * host at the root and the editor at /game/, which is exactly the state
	 * #953 was opened to end. */
	assert.match(readFileSync(join(out, "index.html"), "utf8"), /editor/);
	assert.match(
		readFileSync(join(out, ENGINE_PREFIX, "index.html"), "utf8"),
		/locateFile/
	);
});

test("the editor's output does not disturb the engine's", () => {
	const { artifacts } = fakeBuild();
	const withoutEditor = mkdtempSync(join(tmpdir(), "krudd-out-"));
	const withEditor = mkdtempSync(join(tmpdir(), "krudd-out-"));

	const before = stageSite({ artifacts, outDir: withoutEditor, stem: STEM });
	const after = stageSite({
		artifacts,
		outDir: withEditor,
		stem: STEM,
		editorDir: fakeEditor(),
	});

	assert.deepEqual(
		after.filter((name) => name !== "./"),
		before
	);
	assert.equal(
		readFileSync(join(withEditor, ENGINE_PREFIX, "index.html"), "utf8"),
		readFileSync(join(withoutEditor, ENGINE_PREFIX, "index.html"), "utf8")
	);
});

test("stages nothing at the root when the editor is not built", () => {
	/* stageSite still tolerates a null editor, because it is a pure staging
	 * function and this is a real state a local build reaches. It is
	 * scripts/build.mjs that refuses to ship it — a site with no root document
	 * is a 404 on the way in, and that judgement belongs to the deploy step
	 * rather than to the thing that copies files. */
	const { artifacts } = fakeBuild();
	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));

	const staged = stageSite({ artifacts, outDir: out, stem: STEM, editorDir: null });

	assert.ok(!staged.includes("./"));
	assert.deepEqual(readdirSync(out), [ENGINE_PREFIX]);
});
