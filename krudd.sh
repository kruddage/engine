#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# krudd bootstrap
#   ./krudd.sh              resolve projects here (setup / run / pick)
#   ./krudd.sh build        configure + build, no prompts (what CI runs)
#   ./krudd.sh new-project  scaffold a new project
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Pick up the environment setup.sh wrote (KRUDD_DAWN_PREFIX, and the Qt vars if
# Qt6 was found), so `./krudd.sh editor` works after a one-time `./setup.sh`
# with no manual exports. It uses `:=` assignment, so anything already set in
# the caller's shell still wins.
if [ -f "$root/.krudd-env" ]; then
	. "$root/.krudd-env"
fi

# System default compiler — CC if set, else the first of cc/gcc/clang found.
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
	echo "krudd.sh: no C compiler found (set CC, or install cc/gcc/clang)" >&2
	exit 1
fi

echo "krudd.sh: found C compiler $cc"

# s7.c/s7.h are vendored (see krudd/third_party/s7.artifact); sync.sh checks
# their checksum before we compile the tool that embeds them.
. "$root/krudd/third_party/sync.sh"

bin="$root/krudd/krudd"
src="$root/krudd/krudd.c"
s7="$root/krudd/third_party/s7.c"
if [ ! -x "$bin" ] || [ "$src" -nt "$bin" ] || [ "$s7" -nt "$bin" ]; then
	"$cc" -O2 -w -DWITH_C_LOADER=0 -DWITH_MAIN=0 \
		-I"$root/krudd/third_party" \
		-o "$bin" "$src" "$s7" -lm
fi

export KRUDD_ROOT="$root"
exec "$bin" "$@"
