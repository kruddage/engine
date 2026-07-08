#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# krudd Scheme test harness: build a native s7 interpreter and run the checks
# that need only s7 (no WASM toolchain) — today the de-verbatimed root bootstrap
# (introspect_test.scm). The Ninja backend has its own harness at
# krudd/build/ninja/run-tests.sh.
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
[ -n "$cc" ] || { echo "krudd: no C compiler (set CC)" >&2; exit 1; }

. "$root/krudd/third_party/sync.sh"

work="$root/build-ninja"
mkdir -p "$work"
s7bin="$work/s7"
s7src="$root/krudd/third_party/s7.c"
if [ ! -x "$s7bin" ] || [ "$s7src" -nt "$s7bin" ]; then
	echo "krudd: building s7 test interpreter"
	"$cc" -O2 -w -DWITH_MAIN=1 -DWITH_C_LOADER=0 \
		-I"$root/krudd/third_party" \
		-o "$s7bin" "$s7src" -lm
fi

"$s7bin" "$root/krudd/build/introspect_test.scm"
