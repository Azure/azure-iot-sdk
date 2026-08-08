#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Build the per-suite and combined coverage reports from gcovr tracefiles.
#
#   combine-coverage.sh <out-dir> <label>=<tracefile> [<label>=<tracefile> ...]
#
# Every tracefile must come from the SAME commit. Tracefiles record file names
# and line numbers, so merging two revisions of a file silently attributes hits
# to lines that have since moved -- a wrong number rather than an error. The
# caller is responsible for checking that; this script only reports what it was
# given, and records the labels in the output so the combination is auditable.
#
# Produces, in <out-dir>:
#   <label>.summary.json  /  <label>.md    -- one per suite, report-only
#   combined.summary.json /  combined.md   -- the union
#   combined.cobertura.xml, combined.lcov  -- for downstream tooling
#
# coverage_report.py runs against every report, so its UNMEASURED check (source
# files on disk that no report mentions) applies per suite as well as combined.

set -o errexit
set -o nounset
set -o pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: ${0##*/} <out-dir> <label>=<tracefile> [...]" 1>&2
    exit 2
fi

out_dir="$1"; shift
c_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
components="${c_root}/tests/coverage-components.json"
reporter="${c_root}/tests/coverage_report.py"

mkdir -p "${out_dir}"

labels=()
add_args=()
pairs=()

for pair in "$@"; do
    label="${pair%%=*}"
    file="${pair#*=}"
    if [ "${label}" = "${pair}" ] || [ -z "${label}" ] || [ -z "${file}" ]; then
        echo "::error::malformed argument '${pair}'; expected <label>=<tracefile>" 1>&2
        exit 2
    fi
    if [ ! -f "${file}" ]; then
        echo "::error::tracefile for '${label}' not found at '${file}'" 1>&2
        exit 1
    fi
    labels+=("${label}")
    pairs+=("${pair}")
    add_args+=(--add-tracefile "${file}")
done

# Validate BEFORE producing anything. The suites feeding this run on separate
# schedules, so tracefiles from different commits is the normal failure, and it
# produces a plausible wrong number rather than an error. Refuse instead.
echo "--- verifying all tracefiles came from the same source ---"
python3 "${c_root}/tests/coverage_contribution.py" \
    --output "${out_dir}/contribution.md" "${pairs[@]}"

for pair in "${pairs[@]}"; do
    label="${pair%%=*}"
    file="${pair#*=}"

    echo "--- per-suite report: ${label} ---"
    gcovr --root "${c_root}" --add-tracefile "${file}" \
          --json-summary "${out_dir}/${label}.summary.json" --json-summary-pretty \
          --print-summary > /dev/null
    # Report-only per suite. A single leg legitimately never touches large parts
    # of the tree -- the CSR e2e run has no reason to enter the ADU client -- so
    # enforcing here would be noise. The combined report is the one that gates.
    python3 "${reporter}" \
        --summary "${out_dir}/${label}.summary.json" \
        --components "${components}" \
        --source-root "${c_root}" \
        --output "${out_dir}/${label}.md" > /dev/null || true
done

echo "--- combined report (${#labels[@]} suites: ${labels[*]}) ---"
# --root must match the root the tracefiles were produced with (collect-coverage.sh
# uses c/). The lcov and cobertura writers open the source files, so a mismatched
# root fails with a confusing FileNotFoundError rather than a path error.
gcovr --root "${c_root}" "${add_args[@]}" \
      --json-summary "${out_dir}/combined.summary.json" --json-summary-pretty \
      --cobertura "${out_dir}/combined.cobertura.xml" --cobertura-pretty \
      --lcov "${out_dir}/combined.lcov" \
      --print-summary

python3 "${reporter}" \
    --summary "${out_dir}/combined.summary.json" \
    --components "${components}" \
    --source-root "${c_root}" \
    --output "${out_dir}/combined.md" || true

# Prepend the by-suite table to the contribution report written above.
python3 - "${out_dir}" "${labels[@]}" <<'PY' > "${out_dir}/by-suite.md"
import json, os, sys
out_dir = sys.argv[1]; labels = sys.argv[2:]
load = lambda p: json.load(open(p))
combined = load(os.path.join(out_dir, "combined.summary.json"))
print("## Coverage by suite\n")
print("| Suite | Line % | Branch % | Function % | Lines covered |")
print("|---|---:|---:|---:|---:|")
for label in labels:
    s = load(os.path.join(out_dir, label + ".summary.json"))
    print("| {} | {:.1f}% | {:.1f}% | {:.1f}% | {} |".format(label, s["line_percent"], s["branch_percent"], s.get("function_percent", 0.0), s["line_covered"]))
print("| **combined** | **{:.1f}%** | **{:.1f}%** | **{:.1f}%** | **{}** |".format(combined["line_percent"], combined["branch_percent"], combined.get("function_percent", 0.0), combined["line_covered"]))
PY
# Blank line before appending, or the heading that follows the table is not
# recognised as a heading.
echo "" >> "${out_dir}/by-suite.md"
cat "${out_dir}/contribution.md" >> "${out_dir}/by-suite.md"
mv "${out_dir}/by-suite.md" "${out_dir}/contribution.md"


echo
echo "reports written to ${out_dir}:"
ls -1 "${out_dir}" | sed 's/^/  /'
