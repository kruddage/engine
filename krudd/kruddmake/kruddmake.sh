#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# kruddmake — the build entry point.
#
#   kruddmake              resolve projects here (setup / run / pick)
#   kruddmake build        configure + build, no prompts (what CI runs)
#   kruddmake run          build, then serve the site
#   kruddmake new-project  scaffold a new project
#
# This is the whole of kruddmake's public surface: the operations, not the five
# Scheme modules behind them (#920). It builds the krudd host tool if it is
# missing or stale and execs it — the dispatch and the build itself live in
# krudd.c and in the .scm files beside this script.
#
# Deliberately plain POSIX shell with no node in the path. This path is the
# entry point, not a convenience over one: a contributor building or testing the
# engine runs this script, and nothing above it in the tree has to exist. That
# @kruddage/engine can also invoke it — by this path, which is the only reach
# into krudd/ the workspace's boundary check permits anyone (#934) — is the
# second door, not the door (WORKSPACE.md, Q2).
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

# System default compiler — CC if set, else the first of cc/gcc/clang found.
# This builds the krudd host tool itself; the engine's own output is WASM, from
# emcc (see README.md, "Building").
cc=${CC:-}
if [ -z "$cc" ]; then
	for c in clang gcc cc; do
		if command -v "$c" >/dev/null 2>&1; then
			cc=$c
			break
		fi
	done
fi
if [ -z "$cc" ]; then
	echo "kruddmake: no C compiler found (set CC, or install cc/gcc/clang)" >&2
	exit 1
fi

echo "kruddmake: found C compiler $cc"

# We always build against the latest kruddage/s7 release (see
# krudd/third_party/s7.artifact); sync.sh fetches + checksum-verifies the
# prebuilt header/library before we link the tool that embeds them, and
# exports S7_HEADER / S7_NATIVE_LIB.
. "$root/krudd/third_party/sync.sh"

bin="$root/krudd/krudd"
src="$root/krudd/krudd.c"
# The WITH_* defines keep krudd.c's view of s7.h identical to how the linked
# library was built (no dlopen C-loader, s7 as a library rather than a REPL).
if [ ! -x "$bin" ] || [ "$src" -nt "$bin" ] || [ "$S7_NATIVE_LIB" -nt "$bin" ]; then
	"$cc" -O2 -w -DWITH_C_LOADER=0 -DWITH_MAIN=0 \
		-I"$root/krudd/third_party" \
		-o "$bin" "$src" "$S7_NATIVE_LIB" -lm
fi

export KRUDD_ROOT="$root"
exec "$bin" "$@"
