#!/usr/bin/env python3
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

"""Summarize and gate gcovr coverage output per component.

Reads a gcovr --json-summary report plus coverage-components.json, aggregates
per component, and emits a Markdown table. Two checks are applied:

  1. Per-component floors. A component whose "line"/"branch" entry is null is
     measured and reported but not gated.
  2. Denominator assertion. Every .c file under the component prefixes must
     appear in the report. A shipped adapter dropping out of the build would
     otherwise RAISE the reported number instead of lowering it.

Without --enforce the script only reports (exit 0). With --enforce, either
check failing exits non-zero.
"""

import argparse
import json
import os
import sys


def load_components(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def aggregate(files, prefix):
    """Sum raw counters for files under prefix. Percentages are derived from the
    totals rather than averaged, so large files are not weighted equally with
    small ones."""
    totals = {"line_total": 0, "line_covered": 0, "branch_total": 0, "branch_covered": 0}
    matched = []
    for entry in files:
        if entry["filename"].startswith(prefix):
            matched.append(entry["filename"])
            for key in totals:
                totals[key] += entry.get(key, 0) or 0
    return totals, matched


def percent(covered, total):
    return 100.0 * covered / total if total else None


def fmt(value):
    return "n/a" if value is None else "{:.1f}%".format(value)


def verdict(value, floor):
    if floor is None:
        return "-"
    if value is None:
        return "n/a"
    return "PASS" if value >= floor else "**FAIL**"


def discover_sources(source_root, prefix):
    """Every .c file on disk under prefix, relative to source_root."""
    found = []
    base = os.path.join(source_root, prefix)
    for dirpath, _dirnames, filenames in os.walk(base):
        for name in filenames:
            if name.endswith(".c"):
                rel = os.path.relpath(os.path.join(dirpath, name), source_root)
                found.append(rel.replace(os.sep, "/"))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", required=True, help="gcovr --json-summary output")
    parser.add_argument("--components", required=True, help="coverage-components.json")
    parser.add_argument("--source-root", required=True, help="the c/ project directory")
    parser.add_argument("--output", help="write the Markdown table here as well as stdout")
    parser.add_argument("--enforce", action="store_true",
                        help="exit non-zero when a floor is missed or a source file is unmeasured")
    args = parser.parse_args()

    with open(args.summary, encoding="utf-8") as handle:
        report = json.load(handle)

    files = report.get("files", [])
    components = load_components(args.components)

    rows = []
    failures = []
    unmeasured = []

    for comp in components:
        name = comp["name"]
        prefix = comp["prefix"]
        totals, matched = aggregate(files, prefix)

        line_pct = percent(totals["line_covered"], totals["line_total"])
        branch_pct = percent(totals["branch_covered"], totals["branch_total"])

        # Denominator assertion: compare what is on disk to what was measured.
        on_disk = set(discover_sources(args.source_root, prefix))
        missing = sorted(on_disk - set(matched))
        if missing:
            unmeasured.extend(missing)

        rows.append({
            "name": name,
            "files": len(matched),
            "lines": totals["line_total"],
            "line_pct": line_pct,
            "line_floor": comp.get("line"),
            "branch_pct": branch_pct,
            "branch_floor": comp.get("branch"),
            "missing": len(missing),
        })

        for metric, value, floor in (("line", line_pct, comp.get("line")),
                                     ("branch", branch_pct, comp.get("branch"))):
            if floor is not None and value is not None and value < floor:
                failures.append(
                    "{}: {} coverage {:.1f}% is below the {:.1f}% floor".format(
                        name, metric, value, floor))

    overall_line = percent(report.get("line_covered", 0), report.get("line_total", 0))
    overall_branch = percent(report.get("branch_covered", 0), report.get("branch_total", 0))

    lines = []
    lines.append("## C SDK code coverage")
    lines.append("")
    lines.append("| Component | Files | Lines | Line % | Floor | Branch % | Floor | Status |")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | :---: |")
    for row in rows:
        status = verdict(row["line_pct"], row["line_floor"])
        if row["missing"]:
            status = "**UNMEASURED**"
        lines.append("| `{}` | {} | {} | {} | {} | {} | {} | {} |".format(
            row["name"], row["files"], row["lines"],
            fmt(row["line_pct"]),
            "-" if row["line_floor"] is None else "{:.0f}%".format(row["line_floor"]),
            fmt(row["branch_pct"]),
            "-" if row["branch_floor"] is None else "{:.0f}%".format(row["branch_floor"]),
            status))
    lines.append("| **total** | {} | {} | **{}** | | **{}** | | |".format(
        len(files), report.get("line_total", 0), fmt(overall_line), fmt(overall_branch)))
    lines.append("")

    if unmeasured:
        lines.append("### Unmeasured source files")
        lines.append("")
        lines.append("These files exist on disk but are absent from the coverage report, "
                     "which means they were not built. Shipped code must not leave the "
                     "denominator.")
        lines.append("")
        for path in sorted(unmeasured):
            lines.append("- `{}`".format(path))
        lines.append("")

    if failures:
        lines.append("### Floor violations")
        lines.append("")
        for failure in failures:
            lines.append("- {}".format(failure))
        lines.append("")

    if not any(comp.get("line") is not None for comp in components):
        lines.append("_Floors are not yet configured; this run is report-only._")
        lines.append("")

    text = "\n".join(lines)
    print(text)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as handle:
            handle.write(text + "\n")

    if not args.enforce:
        return 0

    for path in sorted(unmeasured):
        print("::error::source file not measured by coverage: {}".format(path), file=sys.stderr)
    for failure in failures:
        print("::error::{}".format(failure), file=sys.stderr)

    return 1 if (unmeasured or failures) else 0


if __name__ == "__main__":
    sys.exit(main())
