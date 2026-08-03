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

import { planSite, stageSite } from "../src/stage.mjs";

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

	assert.deepEqual(readdirSync(out).sort(), [
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
	const html = readFileSync(join(out, "index.html"), "utf8");

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
	const html = readFileSync(join(out, "index.html"), "utf8");

	assert.match(html, /<span class="error-source">index\.wasm<\/span>/);
});

test("the staged wasm name is the one locateFile will request", () => {
	/* The end-to-end invariant. locateFile turns a request for "index.wasm"
	 * into "index.<stem>.wasm"; staging must have written exactly that. */
	const { artifacts } = fakeBuild();
	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));

	stageSite({ artifacts, outDir: out, stem: STEM });

	const html = readFileSync(join(out, "index.html"), "utf8");
	const hook = html.match(/'\.([^.']+)\.wasm'/);
	assert.ok(hook, "staged HTML kept its locateFile hook");

	const requested = `index.${hook[1]}.wasm`;
	assert.ok(
		readdirSync(out).includes(requested),
		`locateFile requests ${requested}, staged: ${readdirSync(out).join(", ")}`
	);
});

test("the PWA files keep their literal names", () => {
	/* sw.js is registered by name and manifest.webmanifest is linked by name;
	 * renaming either breaks the reference. */
	const { artifacts } = fakeBuild();
	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));

	stageSite({ artifacts, outDir: out, stem: STEM });
	const staged = readdirSync(out);

	for (const name of ["sw.js", "manifest.webmanifest", "icon-192.png"]) {
		assert.ok(staged.includes(name), `${name} kept its name`);
	}
});

test("copies the runtime asset directory when there is one", () => {
	/* The real shape of it: the project sources a build ships staged, plus the
	 * index naming them that the shell's Load Project control reads. Both are
	 * fetched by literal path — the index by name, each project by the name the
	 * index gave it — so both have to arrive under assets/ unrenamed. */
	const { dir, artifacts } = fakeBuild();
	const assets = join(dir, "assets");
	mkdirSync(assets);
	writeFileSync(join(assets, "projects.json"), '["demo.scm"]\n');
	writeFileSync(join(assets, "demo.scm"), '(project "Demo")\n');

	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));
	const staged = stageSite({
		artifacts,
		outDir: out,
		stem: STEM,
		assetDir: assets,
	});

	assert.ok(staged.includes("assets/"));
	assert.equal(
		readFileSync(join(out, "assets", "projects.json"), "utf8"),
		'["demo.scm"]\n'
	);
	assert.equal(
		readFileSync(join(out, "assets", "demo.scm"), "utf8"),
		'(project "Demo")\n'
	);
});

test("fails when the project index names a file that was not staged", () => {
	/* The index is fetched first and everything it lists is fetched next, so an
	 * index out of step with the directory is a 404 the user meets by clicking
	 * Load Project — the class of failure that used to reach the browser and
	 * nowhere else. */
	const { dir, artifacts } = fakeBuild();
	const assets = join(dir, "assets");
	mkdirSync(assets);
	writeFileSync(join(assets, "projects.json"), '["demo.scm","gone.scm"]\n');
	writeFileSync(join(assets, "demo.scm"), '(project "Demo")\n');

	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));
	assert.throws(
		() => stageSite({ artifacts, outDir: out, stem: STEM, assetDir: assets }),
		/gone\.scm/
	);
});

test("an asset directory with no project index stages unchecked", () => {
	/* A build that staged no project promises nothing about assets/. */
	const { dir, artifacts } = fakeBuild();
	const assets = join(dir, "assets");
	mkdirSync(assets);
	writeFileSync(join(assets, "note.txt"), "no projects here");

	const out = mkdtempSync(join(tmpdir(), "krudd-out-"));
	stageSite({ artifacts, outDir: out, stem: STEM, assetDir: assets });

	assert.equal(
		readFileSync(join(out, "assets", "note.txt"), "utf8"),
		"no projects here"
	);
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
