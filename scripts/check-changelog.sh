#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fails a PR that changes non-exempt files without adding an entry to
# CHANGELOG.md. Internal-only changes — tests, CI config, docs — are exempt by
# path; anything else (a pure refactor, say) can still opt out by putting
# "[no changelog]" in the PR title or description.
#
# Required environment:
#   BASE_SHA   - base commit SHA of the PR (diff start)
#   HEAD_SHA   - head commit SHA of the PR (diff end)
#   PR_TITLE   - PR title
#   PR_BODY    - PR description (may be empty)
#
# Usage: scripts/check-changelog.sh
#        Run from a checkout with enough history to diff BASE_SHA..HEAD_SHA
#        (i.e. actions/checkout with fetch-depth: 0).

set -e

: "${BASE_SHA:?BASE_SHA is required}"
: "${HEAD_SHA:?HEAD_SHA is required}"

case "$PR_TITLE $PR_BODY" in
*'[no changelog]'*)
	printf "OK: PR marked [no changelog], skipping\n"
	exit 0
	;;
esac

changed=$(git diff --name-only "$BASE_SHA" "$HEAD_SHA")

if [ -z "$changed" ]; then
	printf "OK: no changed files\n"
	exit 0
fi

if printf '%s\n' "$changed" | grep -qx 'CHANGELOG.md'; then
	printf "OK: CHANGELOG.md updated\n"
	exit 0
fi

# Exempt: CI config, docs, and test sources. Everything else is treated as
# user-facing enough to require a changelog entry. The exempt set is pinned by
# scripts/check-changelog.test.sh — update both together.
non_exempt=$(printf '%s\n' "$changed" | grep -vE \
	'^\.github/|^docs/|^README\.md$|^CODING_STANDARD\.md$|^CLAUDE\.md$|(_test\.(c|mjs|sh)|\.test\.(mjs|sh))$' \
	|| true)

if [ -z "$non_exempt" ]; then
	printf "OK: only internal-only files changed (tests/CI/docs)\n"
	exit 0
fi

printf "FAIL: this PR changes non-exempt files but does not update CHANGELOG.md\n"
printf "%s\n" "$non_exempt"
printf "\nAdd a bullet under the [Unreleased] heading in CHANGELOG.md, or add\n"
printf "\"[no changelog]\" to the PR title/description if this is genuinely\n"
printf "internal-only (e.g. a refactor with no user-visible effect).\n"
exit 1
