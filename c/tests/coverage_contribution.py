#!/usr/bin/env python3
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
"""Validate that coverage tracefiles are mergeable, and report what each suite adds.

    coverage_contribution.py --output <md> <label>=<tracefile> [<label>=<tracefile> ...]

Two jobs, and the first is the important one.

1. SAME-SOURCE VALIDATION. A gcovr tracefile records file names and line
   numbers. Merging tracefiles produced from different revisions of a file
   attributes hits to lines that have since moved, which yields a plausible but
   wrong percentage rather than an error. The suites that feed this run on
   separate schedules, so drifting apart is the normal case, not an exotic one.

   gcovr stores an md5 of each source line alongside its counter. Two tracefiles
   that disagree on the md5 for the same file and line were built from different
   sources, so merging them is refused. This checks the content itself rather
   than trusting CI metadata about which commit a run used.

2. UNIQUE CONTRIBUTION. Lines this suite covers that no other suite covers --
   what would actually be lost by dropping it. Measured per line, not per file:
   at file granularity every answer is zero, because the unit suite touches
   every file and so nothing else is ever uniquely responsible for one.
"""

import argparse
import collections
import json
import sys


def load(path):
    with open(path) as handle:
        return json.load(handle)


def index(doc):
    """{filename: {line_number: (count, md5)}}"""
    out = {}
    for entry in doc.get("files", []):
        name = entry.get("file")
        if name is None:
            continue
        lines = {}
        for line in entry.get("lines", []):
            lines[line["line_number"]] = (line.get("count", 0), line.get("gcovr/md5"))
        out[name] = lines
    return out


def check_same_source(suites, limit=10):
    """Return a list of human-readable mismatches (empty when mergeable)."""
    seen = {}
    mismatches = []
    for label, files in suites:
        for name, lines in files.items():
            for lineno, (_, md5) in lines.items():
                if md5 is None:
                    continue
                key = (name, lineno)
                prior = seen.get(key)
                if prior is None:
                    seen[key] = (md5, label)
                elif prior[0] != md5:
                    mismatches.append(
                        "{}:{} differs between '{}' and '{}'".format(
                            name, lineno, prior[1], label))
                    if len(mismatches) >= limit:
                        return mismatches
    return mismatches


def unique_lines(suites):
    """{label: (unique_covered_lines, total_covered_lines)}"""
    covered = {label: {(n, ln) for n, lines in files.items()
                       for ln, (count, _) in lines.items() if count > 0}
               for label, files in suites}
    result = collections.OrderedDict()
    for label, mine in covered.items():
        others = set()
        for other_label, theirs in covered.items():
            if other_label != label:
                others |= theirs
        result[label] = (len(mine - others), len(mine))
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, help="write the Markdown table here")
    parser.add_argument("pairs", nargs="+", metavar="LABEL=TRACEFILE")
    args = parser.parse_args()

    suites = []
    for pair in args.pairs:
        if "=" not in pair:
            parser.error("expected LABEL=TRACEFILE, got '{}'".format(pair))
        label, path = pair.split("=", 1)
        suites.append((label, index(load(path))))

    mismatches = check_same_source(suites)
    if mismatches:
        print("::error::coverage tracefiles were not produced from the same source; "
              "merging them would report a wrong number rather than fail.",
              file=sys.stderr)
        for line in mismatches:
            print("  {}".format(line), file=sys.stderr)
        print("  Re-run the contributing suites against a single commit.", file=sys.stderr)
        return 1

    uniq = unique_lines(suites)
    rows = ["## Unique contribution", "",
            "Lines covered by this suite that no other suite covers -- what would be "
            "lost by dropping it.", "",
            "| Suite | Covered lines | Unique to this suite |",
            "|---|---:|---:|"]
    for label, (unique, total) in uniq.items():
        share = (100.0 * unique / total) if total else 0.0
        rows.append("| {} | {} | {} ({:.1f}%) |".format(label, total, unique, share))
    rows.append("")
    rows.append("_All {} tracefiles verified to come from the same source "
                "(per-line md5 agreement)._".format(len(suites)))
    rows.append("")

    text = "\n".join(rows)
    with open(args.output, "w") as handle:
        handle.write(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
