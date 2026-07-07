#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests for check-plugin-no-em-asm.sh. Builds throwaway source trees and asserts
# the checker's exit status, so a change to the grep that stops catching a real
# EM_ASM_* call (or starts flagging prose) fails here instead of in production.
#
# Usage: sh scripts/check-plugin-no-em-asm.test.sh

set -e

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SCRIPT="$HERE/check-plugin-no-em-asm.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail=0
pass=0

# assert_status EXPECTED DIR DESCRIPTION
assert_status() {
	expected="$1"
	dir="$2"
	desc="$3"
	status=0
	sh "$SCRIPT" "$dir" >/dev/null 2>&1 || status=$?
	if [ "$status" -eq "$expected" ]; then
		pass=$((pass + 1))
		printf "ok   %s\n" "$desc"
	else
		fail=$((fail + 1))
		printf "FAIL %s (expected exit %s, got %s)\n" "$desc" "$expected" "$status"
	fi
}

# Clean tree: an ordinary plugin source with no EM_ASM.
mkdir -p "$TMP/clean/sub"
printf 'void f(void) { do_thing(); }\n' >"$TMP/clean/sub/plugin.c"
assert_status 0 "$TMP/clean" "clean tree passes"

# An EM_ASM invocation must be caught.
mkdir -p "$TMP/dirty"
printf 'void g(void) { EM_ASM({ console.log(1); }); }\n' >"$TMP/dirty/bad.c"
assert_status 1 "$TMP/dirty" "EM_ASM( invocation is flagged"

# A suffixed variant with whitespace before the paren must also be caught.
mkdir -p "$TMP/dirty2"
printf 'int g(void) { return EM_ASM_INT ( { return 2; } ); }\n' >"$TMP/dirty2/bad.c"
assert_status 1 "$TMP/dirty2" "EM_ASM_INT with space before paren is flagged"

# A header mention must be caught too (headers are scanned).
mkdir -p "$TMP/dirty_h"
printf '#define WRAP() EM_ASM_PTR({ return 0; })\n' >"$TMP/dirty_h/x.h"
assert_status 1 "$TMP/dirty_h" "EM_ASM_PTR in a header is flagged"

# Prose that merely names the macro (no call paren) must NOT trip the check.
mkdir -p "$TMP/prose"
printf '/* Do not use EM_ASM here; prefer EM_JS. */\nvoid h(void){}\n' >"$TMP/prose/ok.c"
assert_status 0 "$TMP/prose" "prose mentioning EM_ASM does not trip"

# A missing directory is a usage error (exit 2), not a pass.
assert_status 2 "$TMP/does-not-exist" "missing directory exits 2"

printf "\n%d passed, %d failed\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
