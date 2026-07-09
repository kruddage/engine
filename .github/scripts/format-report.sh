#!/bin/sh
# Emit a Markdown report of the style issues that clang-format CANNOT auto-fix.
#
# clang-format handles whitespace, braces, and wrapping — but not naming,
# typedef policy, or comment style. checkpatch.pl (for C) and the .scm comment
# linter (for Scheme) flag those, and this script folds their output into one
# Markdown block. The format janitor drops the block into the PR body and the
# Actions step summary so a human can address the findings by hand; nothing
# here modifies files.
#
# The vendored S7 interpreter under krudd/third_party/ is not our code, so it
# is excluded from every check.
#
# Usage: format-report.sh   (run from the repo root)
#   CHECKPATCH  path to checkpatch.pl (default: /tmp/checkpatch.pl)
set -eu

CHECKPATCH=${CHECKPATCH:-/tmp/checkpatch.pl}

c_files=$(git ls-files '*.c' '*.h' | grep -v '^krudd/third_party/' || true)
scm_files=$(git ls-files '*.scm' '*.kscm' || true)

echo "## Style report"
echo
echo "Issues below are **not** auto-fixed — resolve them in a follow-up change."
echo

echo "### checkpatch.pl — C, kernel style"
echo
if [ -z "$c_files" ]; then
	echo "_No C files to check._"
elif [ ! -r "$CHECKPATCH" ]; then
	echo "_Skipped — checkpatch.pl not found at \`$CHECKPATCH\`._"
else
	# --no-tree: run standalone, outside a kernel source tree.
	# --file:    treat the arguments as source files, not patches.
	# The --ignore list drops checks that only make sense for patches
	# submitted to the kernel mailing list, not for a standalone repo.
	out=$(printf '%s\n' $c_files | xargs perl "$CHECKPATCH" \
		--no-tree --terse --show-types --strict --file \
		--ignore FILE_PATH_CHANGES,GERRIT_CHANGE_ID,LINUX_VERSION_CODE \
		2>&1 || true)
	if [ -n "$out" ]; then
		printf '```\n%s\n```\n' "$out"
	else
		echo "No findings. :tada:"
	fi
fi
echo

echo "### scm comment lint — Scheme"
echo
if [ -z "$scm_files" ]; then
	echo "_No Scheme files to check._"
else
	out=$(printf '%s\n' $scm_files \
		| xargs python3 .github/scripts/lint-scm-comments.py 2>&1 || true)
	printf '```\n%s\n```\n' "$out"
fi
