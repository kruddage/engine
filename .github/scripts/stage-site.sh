#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Stages only the web-facing build outputs into a clean directory for GitHub
# Pages publishing. The raw build tree must NOT be published wholesale: it can
# carry intermediate artifacts and dependency checkouts that don't belong on
# the Pages branch, so this script whitelists only the files the site needs.
#
# Usage: .github/scripts/stage-site.sh [BUILD_DIR] [OUT_DIR]
#        defaults: BUILD_DIR=build, OUT_DIR=public
#
# All JS and WASM outputs are renamed with the git short hash so browsers
# fetch fresh copies on every deploy.  index.html is patched to match.

set -e

BUILD_DIR="${1:-build}"
OUT_DIR="${2:-public}"

HASH=$(git rev-parse --short HEAD 2>/dev/null || true)
if [ -z "$HASH" ]; then
	HASH="unknown"
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

# Emscripten HTML entry point (fetched with a short cache TTL by GitHub Pages).
cp "$BUILD_DIR/index.html" "$OUT_DIR/"

# JS loader — renamed with the commit hash so stale cached copies are bypassed.
cp "$BUILD_DIR/index.js" "$OUT_DIR/index.${HASH}.js"

# The single WASM module (there are no separate plugin .wasm files) — likewise
# renamed. The glob keeps this robust if a build ever emits more than one.
for wasm in "$BUILD_DIR"/*.wasm; do
	base=$(basename "$wasm" .wasm)
	cp "$wasm" "$OUT_DIR/${base}.${HASH}.wasm"
done

# Rewrite the <script src="index.js"> reference in the HTML to match.
sed -i "s/index\.js/index.${HASH}.js/" "$OUT_DIR/index.html"

# Runtime assets fetched by the asset plugin, if the build produced any.
if [ -d "$BUILD_DIR/assets" ]; then
	cp -r "$BUILD_DIR/assets" "$OUT_DIR"/
fi

# PWA static assets — plain copies from core/, not hashed like the JS/WASM
# above (see sw.js for why that's safe).
cp "$BUILD_DIR/manifest.webmanifest" "$OUT_DIR/"
cp "$BUILD_DIR/sw.js" "$OUT_DIR/"
cp "$BUILD_DIR/icon-192.png" "$OUT_DIR/"
cp "$BUILD_DIR/icon-512.png" "$OUT_DIR/"

printf "Staged site into %s (hash %s):\n" "$OUT_DIR" "$HASH"
ls -1 "$OUT_DIR"
