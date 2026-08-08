#!/usr/bin/env python3
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

"""Summarize and gate gcovr coverage output per component.

Reads a gcovr --json-summary report plus coverage-components.json, aggregates
per component, and emits a Markdown table. Two checks are applied:

  1. Per-component floors. A component whose "line"/"branch"/"function" entry is
     null is measured and reported but not gated.
  2. Denominator assertion. Every .c file under the component prefixes must
     appear in the report. A shipped adapter dropping out of the build would
     otherwise RAISE the reported number instead of lowering it.

Three metrics are reported. Function coverage is the coarsest but answers a
question the other two cannot: an entirely untested function is invisible in a
healthy line percentage when the rest of its file is well covered, and it is a
different kind of gap from a partially-exercised one.

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
    totals = {"line_total": 0, "line_covered": 0,
              "branch_total": 0, "branch_covered": 0,
              "function_total": 0, "function_covered": 0}
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


def verdict(pairs):
    """Status across every configured floor for a component.

    `pairs` is an iterable of (value, floor); a floor of None means that metric
    is reported but not gated.

    This deliberately considers all three metrics rather than line alone. A
    component can sit above its line floor and below its branch or function
    floor, and a Status column that said PASS while --enforce failed would be
    worse than having no column at all -- it would send someone looking for the
    problem in the wrong place.
    """
    configured = [(value, floor) for value, floor in pairs if floor is not None]
    if not configured:
        return "-"
    if any(value is None for value, _ in configured):
        return "n/a"
    return "PASS" if all(value >= floor for value, floor in configured) else "**FAIL**"


def discover_sources(source_root, prefix):
    """Every .c file on disk under prefix, relative to source_root.

    A missing or misspelled prefix is a hard error rather than an empty result:
    silently returning nothing would satisfy the denominator assertion and print
    a 0-file row, which is precisely the "shipped code stopped being measured"
    failure this script exists to catch.
    """
    base = os.path.join(source_root, prefix)
    if not os.path.isdir(base):
        raise SystemExit(
            "coverage_report: component prefix '{}' does not resolve to a "
            "directory under {} (looked in {}). Fix coverage-components.json.".format(
                prefix, source_root, base))

    found = []
    for dirpath, _dirnames, filenames in os.walk(base):
        for name in filenames:
            if name.endswith(".c"):
                rel = os.path.relpath(os.path.join(dirpath, name), source_root)
                found.append(rel.replace(os.sep, "/"))

    if not found:
        raise SystemExit(
            "coverage_report: component prefix '{}' contains no .c files. "
            "Fix coverage-components.json.".format(prefix))

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
        function_pct = percent(totals["function_covered"], totals["function_total"])

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
            "function_pct": function_pct,
            "function_floor": comp.get("function"),
            "functions": totals["function_total"],
            "functions_uncovered": totals["function_total"] - totals["function_covered"],
            "missing": len(missing),
        })

        for metric, value, floor in (("line", line_pct, comp.get("line")),
                                     ("branch", branch_pct, comp.get("branch")),
                                     ("function", function_pct, comp.get("function"))):
            if floor is not None and value is not None and value < floor:
                failures.append(
                    "{}: {} coverage {:.1f}% is below the {:.1f}% floor".format(
                        name, metric, value, floor))

    overall_line = percent(report.get("line_covered", 0), report.get("line_total", 0))
    overall_branch = percent(report.get("branch_covered", 0), report.get("branch_total", 0))
    overall_function = percent(report.get("function_covered", 0),
                               report.get("function_total", 0))

    lines = []
    lines.append("## C SDK code coverage")
    lines.append("")
    lines.append("| Component | Files | Lines | Line % | Floor | Branch % | Floor | "
                 "Func % | Floor | Status |")
    lines.append("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |")
    for row in rows:
        status = verdict((
            (row["line_pct"], row["line_floor"]),
            (row["branch_pct"], row["branch_floor"]),
            (row["function_pct"], row["function_floor"]),
        ))
        if row["missing"]:
            status = "**UNMEASURED**"
        lines.append("| `{}` | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
            row["name"], row["files"], row["lines"],
            fmt(row["line_pct"]),
            "-" if row["line_floor"] is None else "{:.0f}%".format(row["line_floor"]),
            fmt(row["branch_pct"]),
            "-" if row["branch_floor"] is None else "{:.0f}%".format(row["branch_floor"]),
            fmt(row["function_pct"]),
            "-" if row["function_floor"] is None else "{:.0f}%".format(row["function_floor"]),
            status))
    lines.append("| **total** | {} | {} | **{}** | | **{}** | | **{}** | | |".format(
        len(files), report.get("line_total", 0), fmt(overall_line), fmt(overall_branch),
        fmt(overall_function)))
    lines.append("")

    # A function nothing calls is a different gap from a function called once
    # and only partly walked, and the line percentage hides it whenever the rest
    # of the file is healthy. Name the count so it can be acted on.
    never_called = [row for row in rows if row["functions_uncovered"]]
    if never_called:
        lines.append("### Functions never entered")
        lines.append("")
        lines.append("Counted per component. These are functions no test calls at all, "
                     "which a healthy line percentage can conceal.")
        lines.append("")
        for row in sorted(never_called, key=lambda r: -r["functions_uncovered"]):
            lines.append("- `{}`: {} of {} functions never entered".format(
                row["name"], row["functions_uncovered"], row["functions"]))
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

    if not any(comp.get(metric) is not None
               for comp in components
               for metric in ("line", "branch", "function")):
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
