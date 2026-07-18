#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the dawn-smoke binary against an external native Dawn checkout.
#
# This is deliberately NOT part of `./krudd.sh build`. dawn-smoke's whole job is
# to prove the Dawn build seam works before any engine code depends on it, so it
# must be buildable without the engine build graph having grown a Dawn edge yet.
# See tools/dawn-smoke/README.md for how the same flags map onto kruddmake.
#
#   DAWN_PREFIX=/data/gage/dawn-native/install ./tools/dawn-smoke/build.sh
set -e

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DAWN_PREFIX=${DAWN_PREFIX:-/data/gage/dawn-native/install}

if [ ! -f "$DAWN_PREFIX/lib/libwebgpu_dawn.a" ]; then
	echo "build.sh: no libwebgpu_dawn.a under $DAWN_PREFIX" >&2
	echo "build.sh: see tools/dawn-smoke/README.md for the Dawn build recipe" >&2
	exit 1
fi

cc=${CC:-clang}
# libwebgpu_dawn.a is C++, so the final link must be driven by a C++ driver (or
# hand -lstdc++). Everything else is plain C.
cxx=${CXX:-clang++}

mkdir -p "$here/out"

$cc -std=gnu11 -Wall -Wextra -O2 \
	-I"$DAWN_PREFIX/include" \
	-c "$here/smoke.c" -o "$here/out/smoke.o"

$cxx "$here/out/smoke.o" \
	"$DAWN_PREFIX/lib/libwebgpu_dawn.a" \
	-lz -ldl -lpthread -lm \
	-o "$here/out/dawn-smoke"

echo "build.sh: built $here/out/dawn-smoke"
