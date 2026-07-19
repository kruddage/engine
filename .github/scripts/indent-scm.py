#!/usr/bin/env python3
"""Reindent .scm files with Emacs's scheme-mode indenter (CODING_STANDARD.md's
"Scheme — .scm files" rule: spaces, call arguments aligned under the first
argument), then restore any line that is a continuation of a multi-line
string literal to its original exact content — the indenter otherwise
reindents those as if they were code and corrupts the string.

Usage:
  indent-scm.py <file.scm> [file.scm ...]        reindent in place
  indent-scm.py --check <file.scm> [file.scm ...] report files that would
                                                    change, exit 1 if any do
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SELF_DIR = Path(__file__).resolve().parent
ELISP = SELF_DIR / "indent-scm.el"


def line_string_starts(text):
    """For each line, whether it begins already inside an open string
    literal — i.e. is a continuation line of a multi-line string."""
    lines = text.split("\n")
    starts_in_string = []
    in_string = False
    for line in lines:
        starts_in_string.append(in_string)
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
                i += 1
                continue
            if c == "#" and i + 1 < n and line[i + 1] == "\\":
                i += 3  # skip a #\<char> literal, e.g. #\; or #\"
                continue
            if c == '"':
                in_string = True
                i += 1
                continue
            if c == ";":
                break  # rest of the line is a comment
            i += 1
    return starts_in_string


def reindent(path):
    """Return PATH's content reindented, with string-literal continuation
    lines preserved verbatim."""
    orig_text = path.read_text(encoding="utf-8")
    orig_lines = orig_text.split("\n")
    starts_in_string = line_string_starts(orig_text)

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp) / path.name
        tmp_path.write_text(orig_text, encoding="utf-8")
        subprocess.run(
            ["emacs", "--batch", "-l", str(ELISP), str(tmp_path)],
            check=True,
            capture_output=True,
        )
        new_lines = tmp_path.read_text(encoding="utf-8").split("\n")

    if len(new_lines) != len(orig_lines):
        raise RuntimeError(
            f"{path}: line count changed ({len(orig_lines)} -> {len(new_lines)}), "
            "refusing to reindent"
        )

    for i, in_str in enumerate(starts_in_string):
        if in_str:
            new_lines[i] = orig_lines[i]

    return orig_text, "\n".join(new_lines)


def main(argv):
    check = False
    if argv and argv[0] == "--check":
        check = True
        argv = argv[1:]

    if shutil.which("emacs") is None:
        print("indent-scm: emacs not found on PATH", file=sys.stderr)
        return 1

    changed = []
    for arg in argv:
        path = Path(arg)
        orig_text, new_text = reindent(path)
        if new_text != orig_text:
            changed.append(arg)
            if check:
                print(f"{arg}: not canonically indented")
            else:
                path.write_text(new_text, encoding="utf-8")
                print(f"{arg}: reindented")

    if check:
        if changed:
            print(f"\nindent-scm: {len(changed)} file(s) need reindenting")
            print("Run: python3 .github/scripts/indent-scm.py " + " ".join(changed))
            return 1
        print(f"indent-scm: OK ({len(argv)} file(s))")
        return 0

    print(f"indent-scm: {len(changed)}/{len(argv)} file(s) changed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
