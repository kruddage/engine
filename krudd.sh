#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# krudd bootstrap — krudd's "gradlew". You don't have the krudd binary yet, so
# this compiles it with the system compiler and hands off. Thin on purpose:
# once `krudd` is installed on your PATH this script goes away.
#
#   ./krudd.sh              resolve projects here (setup / run / pick)
#   ./krudd.sh build        configure + build, no prompts (what CI runs)
#   ./krudd.sh new-project  scaffold a new project
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# System default compiler — CC if set, else the first of cc/gcc/clang found.
cc=${CC:-}
if [ -z "$cc" ]; then
	for c in cc gcc clang; do
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

# krudd links the vendored s7 Scheme (third_party/s7.c) as its build
# interpreter. s7 is a big single file, so only recompile when a source is
# newer than the binary — the first build costs ~40s, later runs are instant.
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
