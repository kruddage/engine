#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Render a gcovr JSON summary into the Markdown body of the sticky coverage
# comment the CI `coverage` job posts on each pull request.
#
# This is report-only: it never fails the build on a low number. The floor
# gate is deliberately left out until the baseline has been watched for a
# while (see README "Sanitizer and coverage gates"). Keeping the formatting
# here rather than in the workflow YAML means the exact table can be produced
# and eyeballed locally with the same gcovr JSON the CI job feeds it.
#
# Usage:
#   gcovr ... --json-summary <summary.json>
#   python3 .github/scripts/coverage-report.py <summary.json> <out.md>
#
# The marker comment on the first line lets the workflow find and update its
# own previous comment instead of posting a new one every push.

import collections
import json
import sys

MARKER = "<!-- krudd-coverage-report -->"


def pct(covered, total):
    return (100.0 * covered / total) if total else 100.0


def metric_row(label, data, prefix):
    total = data.get(f"{prefix}_total", 0)
    covered = data.get(f"{prefix}_covered", 0)
    return f"| {label} | {pct(covered, total):.1f}% ({covered} / {total}) |"


def subsystem(path):
    # krudd/engine/<subsystem>/... -> <subsystem>
    parts = path.split("/")
    return parts[2] if len(parts) > 2 else "(root)"


def main():
    summary_path, out_path = sys.argv[1], sys.argv[2]
    with open(summary_path) as fh:
        data = json.load(fh)
    files = data.get("files", [])

    lines = [
        MARKER,
        "## 📊 Native coverage report",
        "",
        "_Report-only — no floor gate yet. Measured over engine production "
        "code (test files and `third_party/` excluded) by the native suite "
        "built with gcc + gcov._",
        "",
        "| Metric | Coverage |",
        "|---|---|",
        metric_row("Lines", data, "line"),
        metric_row("Functions", data, "function"),
        metric_row("Branches", data, "branch"),
        "",
    ]

    # Per-subsystem line coverage, worst first — the actionable view.
    agg = collections.defaultdict(lambda: [0, 0])
    rows = []
    for f in files:
        name = f["filename"]
        lt, lc = f.get("line_total", 0), f.get("line_covered", 0)
        rows.append((pct(lc, lt), lc, lt, name))
        sub = agg[subsystem(name)]
        sub[0] += lc
        sub[1] += lt

    lines += [
        "<details><summary>Per-subsystem line coverage</summary>",
        "",
        "| Subsystem | Lines | Covered / Total |",
        "|---|---:|---:|",
    ]
    for sub in sorted(agg, key=lambda s: pct(agg[s][0], agg[s][1])):
        c, t = agg[sub]
        lines.append(f"| `{sub}` | {pct(c, t):.1f}% | {c} / {t} |")
    lines += ["", "</details>", ""]

    # Lowest-covered files: where new tests would move the number most.
    lowest = sorted(rows)[:15]
    lines += [
        "<details><summary>Lowest-covered files</summary>",
        "",
        "| File | Lines | Covered / Total |",
        "|---|---:|---:|",
    ]
    for p, lc, lt, name in lowest:
        short = name.replace("krudd/engine/", "")
        lines.append(f"| `{short}` | {p:.1f}% | {lc} / {lt} |")
    lines += [
        "",
        "</details>",
        "",
        "> Files at 0% are browser/GPU-only (WebGL/WebGPU backends, plugin and "
        "game entry glue) that can't execute in a headless native test; they "
        "are counted here for a full picture.",
    ]

    with open(out_path, "w") as fh:
        fh.write("\n".join(lines) + "\n")

    # One-line summary for the CI job log.
    print(
        "coverage: lines {:.1f}% functions {:.1f}% branches {:.1f}%".format(
            pct(data.get("line_covered", 0), data.get("line_total", 0)),
            pct(data.get("function_covered", 0), data.get("function_total", 0)),
            pct(data.get("branch_covered", 0), data.get("branch_total", 0)),
        )
    )


if __name__ == "__main__":
    main()
