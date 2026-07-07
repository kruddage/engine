#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# krudd/ninja test harness.
#
# Stage 1 (always): build a native s7 interpreter and run resolve_test.scm —
# the resolver + emitter checks. This needs only a C compiler, no WASM/Ninja.
#
# Stage 2 (when ninja + cc are present): render the real manifest to a
# build.ninja and build the `native` target with ninja(1). Each test links and
# runs as a run_test stamp, so a green `ninja native` means the whole native
# suite passed through the generated build — the proof the Ninja path is
# equivalent to the CMake one for native builds.
set -e

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
export KRUDD_ROOT="$root"

cc=${CC:-}
if [ -z "$cc" ]; then
	for c in cc gcc clang; do
		if command -v "$c" >/dev/null 2>&1; then
			cc=$c
			break
		fi
	done
fi
[ -n "$cc" ] || { echo "krudd/ninja: no C compiler (set CC)" >&2; exit 1; }

work="$root/build-ninja"
mkdir -p "$work"

s7bin="$work/s7"
s7src="$root/krudd/third_party/s7.c"
if [ ! -x "$s7bin" ] || [ "$s7src" -nt "$s7bin" ]; then
	echo "krudd/ninja: building s7 test interpreter"
	"$cc" -O2 -w -DWITH_MAIN=1 -DWITH_C_LOADER=0 \
		-I"$root/krudd/third_party" \
		-o "$s7bin" "$s7src" -lm
fi

# Stage 1: resolver/emitter checks; render build.ninja for stage 2.
export KRUDD_NINJA_OUT="$work/build.ninja"
"$s7bin" "$root/krudd/ninja/resolve_test.scm"

# Stage 2: build + run the native suite through the generated build.ninja.
if command -v ninja >/dev/null 2>&1; then
	echo "krudd/ninja: building native suite via ninja"
	ninja -C "$work" -f build.ninja native
	echo "krudd/ninja: ninja native build + tests OK"
else
	echo "krudd/ninja: ninja(1) not found — skipping native build stage"
fi
