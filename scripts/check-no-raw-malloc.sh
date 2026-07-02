#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Bans direct calls to malloc/free/realloc/calloc outside modules/memory/.
#
# modules/memory/ is the sole allocator seam (it wraps mimalloc and exposes
# mem_alloc/mem_free/mem_pool_* to the rest of the engine). Everything else —
# core modules and plugins alike — must go through that vtable so allocation
# stays swappable (pool allocators, budget tracking, WASM heap accounting).
# A stray libc malloc()/free() bypasses all of that invisibly: it still
# compiles and links, it just silently escapes the engine's allocator.
#
# Usage: scripts/check-no-raw-malloc.sh [DIR ...]
#        DIR defaults to "modules plugins".
#
# Only bare malloc/free/realloc/calloc INVOCATIONS are flagged. A leading
# word character, '.', or '>' excludes the match, so mi_malloc(), xmalloc(),
# and vtable calls like g_mem->free(ptr) or mem->free(ptr) do not trip it.

set -e

DIRS="${*:-modules plugins}"

for d in $DIRS; do
	if [ ! -d "$d" ]; then
		printf "FAIL: %s is not a directory\n" "$d" >&2
		exit 2
	fi
done

PATTERN='(?<![\w.>])(malloc|free|calloc|realloc)\s*\('

violations=$(grep -rnP \
	--include='*.c' --include='*.cpp' \
	--include='*.h' --include='*.hpp' \
	"$PATTERN" $DIRS | grep -v '^modules/memory/' || true)

if [ -n "$violations" ]; then
	printf "FAIL: raw malloc/free/realloc/calloc is banned outside modules/memory/\n"
	printf "%s\n" "$violations"
	printf "\nRoute allocation through modules/memory/ (mem_alloc/mem_free/mem_pool_*)\n"
	printf "or the plugin memory vtable (g_mem->alloc/g_mem->free) instead.\n"
	exit 1
fi

printf "OK: no raw malloc/free/realloc/calloc outside modules/memory/\n"
