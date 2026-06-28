#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Bans EM_ASM_* macros in plugin (side module) sources.
#
# EM_ASM_* silently breaks in Emscripten side modules (SIDE_MODULE=1):
#   1. The inline JS string is registered in the MAIN module's ASM_CONSTS table
#      at link time; a side module's string is never there, so the call lands on
#      the wrong (or an out-of-bounds) entry.
#   2. emscripten_asm_const_* is a JS-library function. If the main module has no
#      EM_ASM_* calls of its own it is absent from asmLibraryArg, and the dynamic
#      linker substitutes a stub that THROWS on invocation.
# Either way there is no compile or link error — only a runtime exception.
#
# The correct pattern for plugins is to define an EM_JS function in
# plugin_abi.c (main module) and call it from the plugin via extern "C". EM_JS
# produces a named JS-library function that lands in asmLibraryArg and is
# reachable by side modules.
#
# Usage: scripts/check-plugin-no-em-asm.sh [DIR]
#        DIR defaults to "plugins".
#
# Only EM_ASM* macro INVOCATIONS (an EM_ASM identifier followed by "(") are
# flagged, so prose that merely mentions the macro name does not trip the check.

set -e

DIR="${1:-plugins}"

if [ ! -d "$DIR" ]; then
	printf "FAIL: %s is not a directory\n" "$DIR" >&2
	exit 2
fi

# Match EM_ASM, EM_ASM_INT, EM_ASM_DOUBLE, EM_ASM_PTR, EM_ASM_ARGS, ... up to an
# opening paren (optional whitespace allowed), i.e. an actual macro call.
PATTERN='EM_ASM[A-Z0-9_]*[[:space:]]*\('

violations=$(grep -rnE \
	--include='*.c' --include='*.cpp' \
	--include='*.h' --include='*.hpp' \
	"$PATTERN" "$DIR" || true)

if [ -n "$violations" ]; then
	printf "FAIL: EM_ASM_* is banned in side module sources under %s/\n" "$DIR"
	printf "%s\n" "$violations"
	printf "\nUse an EM_JS function in modules/core/plugin_abi.c (main module)\n"
	printf "and call it from the plugin via extern \"C\" instead.\n"
	exit 1
fi

printf "OK: no EM_ASM_* usage under %s/\n" "$DIR"
