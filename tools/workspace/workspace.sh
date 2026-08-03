#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# workspace — the JavaScript layer's entry point.
#
#   workspace.sh link         link the workspace (idempotent; every task does it first)
#   workspace.sh build        engine (WASM) then site, in dependency order
#   workspace.sh test         the workspace's own suite — pure Node, no toolchain
#   workspace.sh test:native  the native C suite, through the workspace
#   workspace.sh check        package boundaries
#   workspace.sh clean        drop each package's build output
#
# This replaces pnpm (#1010). It is not a package manager and does not try to
# be one: there are no third-party dependencies to resolve, no versions to
# arbitrate, and no registry to reach. What pnpm was actually doing here was two
# things — making one symlink so `packages/site` can say `@kruddage/engine`
# rather than reach in by relative path, and running a named script in each
# package — and both are below.
#
# Deliberately plain POSIX shell, the same door kruddmake already uses. Node is
# a genuine prerequisite for this layer (unlike the C build, which must never
# need it — WORKSPACE.md, Q2), but it is called to answer JSON questions rather
# than to be the entry point, so the two doors are now the same shape:
#
#   sh krudd/kruddmake/kruddmake.sh build   the C/WASM door
#   sh tools/workspace/workspace.sh build   the JS door
#
# There is no `--filter`. A task runs in every package that declares a script by
# that name, and the script names are already distinct where it matters:
# `check` is only in @kruddage/barriers, `test:native` only in @kruddage/engine,
# `smoke` only in @kruddage/dawn-smoke. Filtering falls out of the declarations
# instead of being a flag on top of them. To address one package's script
# directly, run it directly — `node packages/engine/scripts/build.mjs` is
# shorter than any filter expression, and every script here resolves its paths
# from import.meta.url, so it does not care what the working directory is.
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

usage() {
	echo "usage: workspace.sh <task>" >&2
	echo "  tasks: link, build, test, test:native, check, clean" >&2
	echo "  (any script name declared by a package also works)" >&2
	exit 2
}

# Membership is the filesystem: every directory under packages/ or tools/ that
# declares a package.json. That is the same question tools/barriers reads back
# in packageDirs(), and it is now asked of the filesystem in both places rather
# than of a list one of them could drift from — which is what pnpm-workspace.yaml
# was, and why removing it left nothing to keep in sync.
#
# packages/engine is emitted first because @kruddage/site's build reads the
# artifacts it produces. That is a named order rather than a resolved one: the
# workspace has exactly one dependency edge, and a topological sort over a graph
# with one edge is machinery for a graph that does not exist (WORKSPACE.md, Q1).
# A second edge gets a second line here, and the linker below will already have
# made it resolvable.
package_dirs() {
	if [ -f "$root/packages/engine/package.json" ]; then
		echo "packages/engine"
	fi
	for dir in "$root"/packages/*/ "$root"/tools/*/; do
		[ -f "$dir/package.json" ] || continue
		rel=${dir#"$root"/}
		rel=${rel%/}
		if [ "$rel" != "packages/engine" ]; then
			echo "$rel"
		fi
	done
}

# The one thing pnpm produced that was load-bearing.
#
# `packages/site` imports @kruddage/engine by bare specifier in three files, and
# Node resolves a bare specifier through node_modules. The alternative —
# `../../engine/src/index.mjs` — is exactly what the boundary check's rule 1
# forbids, because it routes around the "exports" map (tools/barriers). So the
# specifier stays bare and something has to make the link; this is that
# something, and it is `ln -s`.
#
# The edges come from each package.json's "dependencies", keyed on the
# `workspace:` prefix, so the declaration stays where the manifests are rather
# than being hardcoded here. Idempotent: `ln -sfn` re-points an existing link
# instead of failing, so this runs before every task and costs nothing.
link_workspace() {
	# Computed first, into a variable, then applied. A `node ... | while read`
	# pipeline would report the *loop's* status, so a node that exited 1 on an
	# undeclared dependency would print its error and be treated as success.
	edges=$(node -e '
const { readdirSync, existsSync, readFileSync } = require("node:fs");
const { join, relative, dirname } = require("node:path");

const root = process.argv[1];
const dirs = [];
for (const parent of ["packages", "tools"]) {
	if (!existsSync(join(root, parent))) continue;
	for (const name of readdirSync(join(root, parent))) {
		const dir = join(parent, name);
		if (existsSync(join(root, dir, "package.json"))) dirs.push(dir);
	}
}

const byName = new Map();
const manifests = new Map();
for (const dir of dirs) {
	const manifest = JSON.parse(readFileSync(join(root, dir, "package.json"), "utf8"));
	manifests.set(dir, manifest);
	if (manifest.name) byName.set(manifest.name, dir);
}

for (const [dir, manifest] of manifests) {
	for (const [dep, range] of Object.entries(manifest.dependencies ?? {})) {
		if (!String(range).startsWith("workspace:")) continue;
		const target = byName.get(dep);
		if (!target) {
			process.stderr.write(`workspace: ${dir} depends on ${dep}, which is not a package here\n`);
			process.exit(1);
		}
		const link = join(root, dir, "node_modules", dep);
		process.stdout.write(`${link}\t${relative(dirname(link), join(root, target))}\n`);
	}
}
' "$root")

	echo "$edges" | while IFS='	' read -r link target; do
		[ -n "$link" ] || continue
		mkdir -p "$(dirname -- "$link")"
		ln -sfn "$target" "$link"
	done
}

# A package's script by name, or empty if it declares none. JSON is Node's
# question to answer, not sh's.
script_for() {
	node -e '
const { readFileSync } = require("node:fs");
const manifest = JSON.parse(readFileSync(process.argv[1], "utf8"));
process.stdout.write((manifest.scripts ?? {})[process.argv[2]] ?? "");
' "$root/$1/package.json" "$2"
}

[ $# -eq 1 ] || usage
task=$1

link_workspace
if [ "$task" = "link" ]; then
	exit 0
fi

# Word splitting on $(package_dirs) rather than a `| while read` pipeline: the
# loop has to run in *this* shell so that `ran` survives it and so that `set -e`
# aborts the run on the first failing script rather than at the end of a
# subshell. No path here contains whitespace.
ran=0
for pkg in $(package_dirs); do
	script=$(script_for "$pkg" "$task")
	[ -n "$script" ] || continue
	echo "workspace: $pkg \$ $script"
	(cd "$root/$pkg" && sh -c "$script")
	ran=$((ran + 1))
done

# A task no package declares is a typo; reporting it beats exiting 0 having
# done nothing at all.
if [ "$ran" -eq 0 ]; then
	echo "workspace: no package declares a \"$task\" script" >&2
	exit 1
fi
