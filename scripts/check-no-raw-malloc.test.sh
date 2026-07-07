#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests for check-no-raw-malloc.sh. Asserts that a raw libc allocator call is
# caught, that the allowed patterns (mi_malloc, xmalloc, vtable calls) are not,
# and that modules/memory/ is exempt.
#
# Usage: sh scripts/check-no-raw-malloc.test.sh

set -e

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SCRIPT="$HERE/check-no-raw-malloc.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail=0
pass=0

# assert_status EXPECTED DESCRIPTION -- DIR...
assert_status() {
	expected="$1"
	desc="$2"
	shift 2
	[ "$1" = "--" ] && shift
	status=0
	( cd "$TMP" && sh "$SCRIPT" "$@" ) >/dev/null 2>&1 || status=$?
	if [ "$status" -eq "$expected" ]; then
		pass=$((pass + 1))
		printf "ok   %s\n" "$desc"
	else
		fail=$((fail + 1))
		printf "FAIL %s (expected exit %s, got %s)\n" "$desc" "$expected" "$status"
	fi
}

# A bare malloc() outside modules/memory/ must be caught.
mkdir -p "$TMP/modules/core"
printf 'void *f(void) { return malloc(8); }\n' >"$TMP/modules/core/a.c"
assert_status 1 "raw malloc() is flagged" -- modules

# free()/calloc()/realloc() likewise.
printf 'void g(void *p) { free(p); }\n' >"$TMP/modules/core/b.c"
assert_status 1 "raw free() is flagged" -- modules

# The same call under modules/memory/ is the allocator seam — exempt.
rm -f "$TMP/modules/core/a.c" "$TMP/modules/core/b.c"
mkdir -p "$TMP/modules/memory"
printf 'void *mem_alloc(unsigned n) { return malloc(n); }\n' >"$TMP/modules/memory/mem.c"
assert_status 0 "malloc inside modules/memory/ is exempt" -- modules

# Prefixed / member-call forms must NOT trip: mi_malloc, xmalloc, g_mem->free.
mkdir -p "$TMP/plugins/x"
cat >"$TMP/plugins/x/ok.c" <<'EOF'
void *a(void) { return mi_malloc(8); }
void *b(void) { return xmalloc(8); }
void  c(void *p, struct mem *g_mem) { g_mem->free(p); }
EOF
assert_status 0 "mi_malloc / xmalloc / vtable free do not trip" -- plugins

# A missing directory is a usage error (exit 2).
assert_status 2 "missing directory exits 2" -- does-not-exist

printf "\n%d passed, %d failed\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
