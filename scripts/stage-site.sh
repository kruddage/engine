#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Stages only the web-facing build outputs into a clean directory for GitHub
# Pages publishing. The raw CMake build tree must NOT be published: it contains
# _deps/ (a FetchContent checkout whose imgui-src is a git gitlink), which makes
# the GitHub Pages branch build fail with "No url found for submodule path".
#
# Usage: scripts/stage-site.sh [BUILD_DIR] [OUT_DIR]
#        defaults: BUILD_DIR=build, OUT_DIR=public

set -e

BUILD_DIR="${1:-build}"
OUT_DIR="${2:-public}"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

# Emscripten entry point and its runtime.
cp "$BUILD_DIR"/index.html "$BUILD_DIR"/index.js "$OUT_DIR"/

# index.wasm plus every plugin SIDE_MODULE .wasm (all emitted at the build root).
cp "$BUILD_DIR"/*.wasm "$OUT_DIR"/

# Runtime assets fetched by the asset plugin, if the build produced any.
if [ -d "$BUILD_DIR/assets" ]; then
	cp -r "$BUILD_DIR/assets" "$OUT_DIR"/
fi

printf "Staged site into %s:\n" "$OUT_DIR"
ls -1 "$OUT_DIR"
