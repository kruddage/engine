#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# krudd/third_party/sync.sh — fetch (and verify) the pinned s7 artifacts.
#
# s7 is no longer vendored as source. This downloads the prebuilt artifacts
# pinned in s7.artifact from the kruddage/s7 GitHub Release and verifies each
# one against the ".sha256" sidecar published beside it, so every build links
# the same bytes CI does. Idempotent: once an artifact is present and matches
# its sidecar checksum, no network I/O happens on the next run.
#
# The artifacts are not committed to this repo (see .gitignore); a fresh
# checkout fetches them on the first build. This needs network the first time —
# the download host is github.com, reachable from CI and normal dev machines.
#
# Sourced (not executed) by krudd.sh and run-tests.sh, before the krudd host
# tool exists to fetch anything for them — so this has to be plain POSIX shell.
# Expects $root (the repo root) to already be set by the sourcing script. On
# success it exports the resolved artifact paths:
#
#   S7_HEADER       s7.h                 (its dir is the -I include path)
#   S7_NATIVE_LIB   libs7-linux-x86_64.a (link into native binaries)
#   S7_WASM_LIB     libs7-wasm32.a       (link into the wasm module)
#   S7_CLI          krudds7-linux-x86_64 (the kruddmake bootstrap interpreter)

s7_dir="$root/krudd/third_party"
# shellcheck source=s7.artifact
. "$s7_dir/s7.artifact"

s7_sha256() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | cut -d ' ' -f 1
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | cut -d ' ' -f 1
	else
		echo "krudd/third_party: no sha256sum or shasum found" >&2
		exit 1
	fi
}

s7_download() {
	# $1 url, $2 dest — returns non-zero on failure without leaving a partial.
	if command -v curl >/dev/null 2>&1; then
		curl -fsSL -o "$2" "$1"
	elif command -v wget >/dev/null 2>&1; then
		wget -q -O "$2" "$1"
	else
		echo "krudd/third_party: no curl or wget found" >&2
		exit 1
	fi
}

# Fetch $2 (asset filename) into $s7_dir, verifying it against its published
# "<asset>.sha256" sidecar. Skips the download when the file is already present
# and matches. Sets $s7_asset_path to the resolved path.
s7_sync_one() {
	name=$1
	dest="$s7_dir/$name"
	url="$S7_BASE_URL/$name"
	sidecar_url="$url.sha256"

	# Read the pinned checksum from the sidecar. The sidecar is small and
	# release-scoped, so re-fetching it to confirm a cached artifact is cheap;
	# it is the integrity anchor for the (uncommitted) binary next to it.
	tmp_sum="$dest.sha256.tmp.$$"
	if ! s7_download "$sidecar_url" "$tmp_sum"; then
		rm -f "$tmp_sum"
		echo "krudd/third_party: failed to fetch checksum for $name from $sidecar_url" >&2
		exit 1
	fi
	# GitHub publishes sidecars in "sha256sum" form ("<hash>  <filename>");
	# take the first field so a differing filename column never trips us up.
	want=$(cut -d ' ' -f 1 "$tmp_sum")
	rm -f "$tmp_sum"
	if [ -z "$want" ]; then
		echo "krudd/third_party: empty checksum in sidecar for $name" >&2
		exit 1
	fi

	if [ -f "$dest" ] && [ "$(s7_sha256 "$dest")" = "$want" ]; then
		s7_asset_path="$dest"
		return 0
	fi

	echo "krudd/third_party: fetching $name ($S7_RELEASE, s7 $S7_VERSION)" >&2
	tmp="$dest.tmp.$$"
	if ! s7_download "$url" "$tmp"; then
		rm -f "$tmp"
		echo "krudd/third_party: failed to fetch $name from $url" >&2
		exit 1
	fi
	got=$(s7_sha256 "$tmp")
	if [ "$got" != "$want" ]; then
		rm -f "$tmp"
		echo "krudd/third_party: $name checksum mismatch (got $got, want $want)" >&2
		exit 1
	fi
	mv "$tmp" "$dest"
	s7_asset_path="$dest"
}

s7_sync_one "$S7_HEADER_ASSET"
S7_HEADER="$s7_asset_path"
s7_sync_one "$S7_NATIVE_LIB_ASSET"
S7_NATIVE_LIB="$s7_asset_path"
s7_sync_one "$S7_WASM_LIB_ASSET"
S7_WASM_LIB="$s7_asset_path"
s7_sync_one "$S7_CLI_ASSET"
S7_CLI="$s7_asset_path"
# The CLI is fetched as a plain asset; make it runnable.
chmod +x "$S7_CLI" 2>/dev/null || true

export S7_HEADER S7_NATIVE_LIB S7_WASM_LIB S7_CLI
