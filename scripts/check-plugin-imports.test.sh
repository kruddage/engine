#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests for check-plugin-imports.sh. The real check reads WASM imports with
# wasm-objdump (WABT), which is not present outside the Emscripten SDK, so the
# behavioural cases self-skip when the tool is absent. The no-tool cases (no
# matching files → pass) always run.
#
# Usage: sh scripts/check-plugin-imports.test.sh

set -e

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SCRIPT="$HERE/check-plugin-imports.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail=0
pass=0

assert_status() {
	expected="$1"
	desc="$2"
	shift 2
	status=0
	sh "$SCRIPT" "$@" >/dev/null 2>&1 || status=$?
	if [ "$status" -eq "$expected" ]; then
		pass=$((pass + 1))
		printf "ok   %s\n" "$desc"
	else
		fail=$((fail + 1))
		printf "FAIL %s (expected exit %s, got %s)\n" "$desc" "$expected" "$status"
	fi
}

# With no .wasm files matching, the loop body is skipped and the check reports
# "all 0 plugins passed" — a clean exit regardless of wasm-objdump.
mkdir -p "$TMP/empty"
assert_status 0 "empty build dir passes" "$TMP/empty"/*.wasm

if command -v wasm-objdump >/dev/null 2>&1; then
	# A trivial module that imports nothing banned must pass.
	if command -v wat2wasm >/dev/null 2>&1; then
		printf '(module (func (export "f")))\n' >"$TMP/clean.wat"
		wat2wasm "$TMP/clean.wat" -o "$TMP/clean.wasm"
		assert_status 0 "module with no banned imports passes" "$TMP/clean.wasm"
	else
		printf "skip (no wat2wasm) module-with-clean-imports case\n"
	fi
else
	printf "skip (no wasm-objdump) behavioural import cases\n"
fi

printf "\n%d passed, %d failed\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
