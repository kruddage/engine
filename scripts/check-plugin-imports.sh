#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Verifies that plugin WASM files do not directly import engine implementation
# symbols that should only be accessed through the subsystem vtable API.
#
# Usage: scripts/check-plugin-imports.sh build/*.wasm
#        or: BUILD_DIR=build scripts/check-plugin-imports.sh
#
# Requires wasm-objdump (part of the WABT toolkit, available in the Emscripten
# SDK via `emsdk install latest`).

set -e

BANNED='mem_alloc\b|mem_alloc_zero\b|mem_free\b|mem_pool_create\b|mem_pool_alloc\b|mem_pool_free\b|mem_pool_destroy\b|log_write\b'

if [ "$#" -gt 0 ]; then
	WASM_FILES="$*"
else
	BUILD_DIR="${BUILD_DIR:-build}"
	WASM_FILES="${BUILD_DIR}"/*.wasm
fi

PASS=0
FAIL=0

for wasm in $WASM_FILES; do
	[ -f "$wasm" ] || continue
	violations=$(wasm-objdump -x "$wasm" 2>/dev/null | \
		grep -E "func\[.*\].*($BANNED)" || true)
	if [ -n "$violations" ]; then
		printf "FAIL: %s imports banned engine symbols:\n%s\n" \
			"$wasm" "$violations"
		FAIL=$((FAIL + 1))
	else
		printf "OK:   %s\n" "$wasm"
		PASS=$((PASS + 1))
	fi
done

if [ "$FAIL" -gt 0 ]; then
	printf "\n%d plugin(s) failed import check; %d passed\n" "$FAIL" "$PASS"
	exit 1
fi

printf "\nAll %d plugin(s) passed import check\n" "$PASS"
