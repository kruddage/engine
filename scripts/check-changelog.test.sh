#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests for check-changelog.sh. Stands up a throwaway git repo, commits a base
# and a head state, and asserts the gate's verdict for each case. This is what
# keeps the exempt-path list honest: a change to the exemption regex that starts
# waving through source files (or starts blocking docs) fails here.
#
# Usage: sh scripts/check-changelog.test.sh

set -e

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SCRIPT="$HERE/check-changelog.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

REPO="$TMP/repo"
mkdir -p "$REPO"
cd "$REPO"
git init -q
git config user.email t@t.t
git config user.name t
git config commit.gpgsign false

# Base commit: a couple of tracked files to diff against.
mkdir -p modules docs
printf 'base\n' >modules/a.c
printf '# changelog\n' >CHANGELOG.md
git add -A
git commit -qm base
BASE=$(git rev-parse HEAD)

fail=0
pass=0

# commit_head writes/removes files via the callback, commits, returns HEAD.
# assert_case EXPECTED TITLE BODY DESCRIPTION -- FILE=CONTENT ...
assert_case() {
	expected="$1"
	title="$2"
	body="$3"
	desc="$4"
	shift 4
	[ "$1" = "--" ] && shift

	git reset -q --hard "$BASE"
	for pair in "$@"; do
		path="${pair%%=*}"
		content="${pair#*=}"
		mkdir -p "$(dirname "$path")"
		printf '%s\n' "$content" >"$path"
	done
	git add -A
	git commit -qm head >/dev/null
	head=$(git rev-parse HEAD)

	status=0
	BASE_SHA="$BASE" HEAD_SHA="$head" PR_TITLE="$title" PR_BODY="$body" \
		sh "$SCRIPT" >/dev/null 2>&1 || status=$?
	if [ "$status" -eq "$expected" ]; then
		pass=$((pass + 1))
		printf "ok   %s\n" "$desc"
	else
		fail=$((fail + 1))
		printf "FAIL %s (expected exit %s, got %s)\n" "$desc" "$expected" "$status"
	fi
}

# Source change without a changelog entry is blocked.
assert_case 1 "feat" "" "source change without CHANGELOG is blocked" \
	-- modules/a.c=changed

# Source change WITH a changelog entry passes.
assert_case 0 "feat" "" "source change with CHANGELOG passes" \
	-- modules/a.c=changed CHANGELOG.md=updated

# Only docs / .github changed → internal-only, passes.
assert_case 0 "docs" "" "docs-only change is exempt" \
	-- docs/x.md=hi .github/workflows/ci.yml=steps

# The [no changelog] escape hatch (in the body) passes.
assert_case 0 "refactor" "no user effect [no changelog]" \
	"[no changelog] escape hatch passes" -- modules/a.c=changed

# A .test.sh file is a test source → exempt (regression guard for the
# exemption-list review in #308).
assert_case 0 "tests" "" ".test.sh is exempt as a test source" \
	-- scripts/check-foo.test.sh=echo

printf "\n%d passed, %d failed\n" "$pass" "$fail"
[ "$fail" -eq 0 ]
