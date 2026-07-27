#!/usr/bin/env python3
"""Fail on any .scm comment that isn't SPDX or a ;;! shebang comment.

Usage: lint-scm-comments.py <file.scm> [file.scm ...]

A full-line comment is allowed only when it begins with the ;;! shebang
marker (leading whitespace is fine), mirroring how a #! shebang opts a line
in. The SPDX license header is exempt. Trailing same-line comments are
rejected outright since they can't carry a shebang.
"""
import sys

SHEBANG = ";;!"
SPDX_PREFIX = "; SPDX-License-Identifier:"


def find_comment_start(line):
    """Index of the first ';' outside a string/char literal, or -1."""
    in_string = False
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if in_string:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_string = False
        elif c == '"':
            in_string = True
        elif c == "#" and i + 1 < n and line[i + 1] == "\\":
            i += 3  # skip a #\<char> literal, e.g. #\; or #\"
            continue
        elif c == ";":
            return i
        i += 1
    return -1


def lint_file(path):
    errors = []
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")

    for lineno, line in enumerate(lines, start=1):
        idx = find_comment_start(line)
        if idx == -1:
            continue

        prefix = line[:idx].strip()
        if prefix != "":
            errors.append((lineno, "trailing comment — move it to its own line with a ;;! shebang"))
            continue

        stripped = line.strip()
        if stripped.startswith(SHEBANG):
            continue
        if stripped.startswith(SPDX_PREFIX):
            continue
        errors.append((lineno, "comment without a ;;! shebang marker"))

    return errors


def main(argv):
    total = 0
    for path in argv:
        for lineno, message in lint_file(path):
            print(f"{path}:{lineno}: {message}")
            total += 1
    if total:
        print(f"\nscm-lint: {total} violation(s)")
        return 1
    print(f"scm-lint: OK ({len(argv)} file(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
