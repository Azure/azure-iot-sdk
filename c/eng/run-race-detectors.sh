#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Runs every test labelled "race" (az_iot_add_cmocka_test(... RACE ...)) under
# a valgrind thread checker.
#
# Usage: eng/run-race-detectors.sh <helgrind|drd> [build-dir]
#   build-dir defaults to build/linux-gcc-debug, relative to c/.
#   VALGRIND overrides the valgrind binary.
#
# A test fails when the tool reports an error, when the test itself fails, or
# when valgrind ends without an ERROR SUMMARY (it aborted, so the run proves
# nothing). Suppressions come only from eng/sanitizers/<tool>.supp, plus
# valgrind's defaults; -s lists every suppression used, so the log shows what
# was hidden. Logs go to <build-dir>/race-logs/<tool>/.

set -euo pipefail

tool="${1:-}"
case "${tool}" in
    helgrind | drd) ;;
    *)
        echo "usage: $0 <helgrind|drd> [build-dir]" >&2
        exit 2
        ;;
esac

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(cd "${2:-${root_dir}/build/linux-gcc-debug}" && pwd)"
supp="${root_dir}/eng/sanitizers/${tool}.supp"
valgrind="${VALGRIND:-valgrind}"
# DRD skips stack variables by default; tests share them across threads.
tool_opts=()
[ "${tool}" != "drd" ] || tool_opts=(--check-stack-var=yes)
log_dir="${build_dir}/race-logs/${tool}"
mkdir -p "${log_dir}"

# One JSON object per test: name, command (array), working directory. Assigned
# first so a failing ctest or jq stops the run (set -e, pipefail).
discovered="$(
    ctest --test-dir "${build_dir}" -L '^race$' --show-only=json-v1 |
        jq -c '.tests[] | {name, command,
                wd: ([(.properties // [])[] | select(.name == "WORKING_DIRECTORY") | .value][0] // "")}'
)"
tests=()
[ -z "${discovered}" ] || mapfile -t tests <<<"${discovered}"
if [ "${#tests[@]}" -eq 0 ]; then
    echo "::error::no test labelled 'race' in ${build_dir}"
    exit 1
fi

# GitHub annotations are one line; escape what would end or break one.
annotate() {
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
        printf '::error::%s\n' "$(printf '%s' "$1" | sed -e 's/%/%25/g' | awk '{printf "%s%%0A", $0}')"
    else
        printf 'ERROR: %s\n' "$1"
    fi
}

status=0
for t in "${tests[@]}"; do
    name="$(jq -r '.name' <<<"${t}")"
    mapfile -t cmd < <(jq -r '.command[]' <<<"${t}")
    wd="$(jq -r '.wd' <<<"${t}")"
    log="${log_dir}/${name}.log"

    echo "::group::${tool}: ${name}"
    rc=0
    (
        [ -z "${wd}" ] || cd "${wd}"
        "${valgrind}" --tool="${tool}" --error-exitcode=42 -s --num-callers=40 \
            "${tool_opts[@]}" --suppressions="${supp}" "${cmd[@]}"
    ) >"${log}" 2>&1 || rc=$?
    cat "${log}"
    echo "::endgroup::"

    if ! grep -q "ERROR SUMMARY:" "${log}"; then
        annotate "${tool}: ${name} did not run to completion (exit ${rc})"
        status=1
    elif [ "${rc}" -eq 42 ]; then
        annotate "${tool}: ${name} reported a threading defect:
$(grep -E '^==[0-9]+==' "${log}" | sed '1,/Command:/d' | head -40)"
        status=1
    elif [ "${rc}" -ne 0 ]; then
        annotate "${tool}: ${name} failed (exit ${rc})"
        status=1
    fi
    echo "${tool}: ${name}: $(grep -h 'ERROR SUMMARY:' "${log}" | tail -n 1 | sed -E 's/^==[0-9]+== //')"
    grep -h 'used_suppression:' "${log}" | sed -E 's/^--[0-9]+-- +/  /' || true
done
exit "${status}"
