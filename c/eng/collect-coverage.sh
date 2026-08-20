#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Collect gcov data from one CI leg into a gcovr tracefile.
#
#   collect-coverage.sh <build-dir> <output.json> <label>
#
# Every leg that measures coverage calls this, so the filters live in exactly
# one place. That matters more than it looks: tracefiles produced with different
# --filter / --exclude sets cannot be meaningfully merged, and the resulting
# combined percentage would be quietly wrong rather than obviously broken.
#
# The output is gcovr's own JSON tracefile format, which is what
# `gcovr --add-tracefile` consumes when the legs are combined.

set -o errexit
set -o nounset
set -o pipefail

if [ "$#" -ne 3 ]; then
    echo "usage: ${0##*/} <build-dir> <output.json> <label>" 1>&2
    exit 2
fi

build_dir="$1"
out_file="$2"
label="$3"

repo_c_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ ! -d "${build_dir}" ]; then
    echo "::error::${label}: build directory '${build_dir}' does not exist" 1>&2
    exit 1
fi

# A run that produced no .gcda is the failure this whole exercise exists to
# catch. Reporting 0% would look like a coverage regression and send someone
# hunting through the wrong code, so distinguish the two causes instead --
# they have completely different fixes.
gcda_count="$(find "${build_dir}" -name '*.gcda' | wc -l)"
if [ "${gcda_count}" -eq 0 ]; then
    gcno_count="$(find "${build_dir}" -name '*.gcno' | wc -l)"
    if [ "${gcno_count}" -eq 0 ]; then
        echo "::error::${label}: '${build_dir}' contains no .gcno either, so it was never" 1>&2
        echo "  instrumented. Either the build has not run yet, or it was configured" 1>&2
        echo "  without AZ_IOT_ENABLE_COVERAGE. This is a configuration bug, not a" 1>&2
        echo "  coverage result." 1>&2
    else
        echo "::error::${label}: ${gcno_count} .gcno but no .gcda, so the build WAS" 1>&2
        echo "  instrumented and the binaries never ran to completion. gcov writes its" 1>&2
        echo "  counters at process exit, so a test that crashed, hung, or was killed" 1>&2
        echo "  by a timeout leaves nothing behind." 1>&2
    fi
    echo "  Either way this is 'no data', which is not the same as 'zero coverage'." 1>&2
    exit 1
fi

mkdir -p "$(dirname "${out_file}")"

gcovr --root "${repo_c_root}" "${build_dir}" \
      --filter "${repo_c_root}/src/" --filter "${repo_c_root}/adapters/" \
      --exclude '.*/_deps/.*' \
      --exclude '.*/tests/.*' \
      --exclude '.*/samples/.*' \
      --json "${out_file}" \
      --print-summary

echo "${label}: ${gcda_count} .gcda -> ${out_file}"
