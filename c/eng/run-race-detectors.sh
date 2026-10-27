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
#   AZ_IOT_RACE_SHARDS (default 4): processes each test also labelled
#     "shardable" is split into (AZ_IOT_TEST_SHARD, tests/support/test_shard.h).
#   AZ_IOT_RACE_JOBS (default: CPU count): processes run at once. valgrind
#     runs a process's threads one at a time, so one process uses one CPU.
#   AZ_IOT_RACE_SHARDS=1 AZ_IOT_RACE_JOBS=1 runs one test at a time, unsplit.
#
# A test fails when the tool reports an error, when the test itself fails, or
# when valgrind ends without an ERROR SUMMARY (it aborted, so the run proves
# nothing). Suppressions come only from eng/sanitizers/<tool>.supp, plus
# valgrind's defaults; -s lists every suppression used, so the log shows what
# was hidden. Logs go to <build-dir>/race-logs/<tool>/, one per process.

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
shards="${AZ_IOT_RACE_SHARDS:-4}"
jobs="${AZ_IOT_RACE_JOBS:-$(nproc)}"
for setting in "AZ_IOT_RACE_SHARDS=${shards}" "AZ_IOT_RACE_JOBS=${jobs}"; do
    if ! [[ "${setting#*=}" =~ ^[1-9][0-9]{0,2}$ ]]; then
        echo "${setting%%=*} must be 1-999, got '${setting#*=}'" >&2
        exit 2
    fi
done
# DRD skips stack variables by default; tests share them across threads.
tool_opts=()
[ "${tool}" != "drd" ] || tool_opts=(--check-stack-var=yes)
log_dir="${build_dir}/race-logs/${tool}"
mkdir -p "${log_dir}"
rm -f "${log_dir}"/*.log "${log_dir}"/*.rc

# One JSON object per test: name, command (array), working directory, whether
# it is shardable. Assigned first so a failing ctest or jq stops the run (set
# -e, pipefail).
discovered="$(
    ctest --test-dir "${build_dir}" -L '^race$' --show-only=json-v1 |
        jq -c '.tests[] | {name, command,
                wd: ([(.properties // [])[] | select(.name == "WORKING_DIRECTORY") | .value][0] // ""),
                shardable: ([(.properties // [])[] | select(.name == "LABELS") | .value[]] | index("shardable") != null)}'
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

# Processes to run, "<test index> <shard spec or ->", shardable ones first:
# they are the slow ones, so starting them first shortens the run.
units=()
for i in "${!tests[@]}"; do
    if [ "$(jq -r '.shardable' <<<"${tests[i]}")" = "true" ] && [ "${shards}" -gt 1 ]; then
        for ((s = 0; s < shards; s++)); do units+=("${i} ${s}/${shards}"); done
    fi
done
for i in "${!tests[@]}"; do
    if [ "$(jq -r '.shardable' <<<"${tests[i]}")" != "true" ] || [ "${shards}" -eq 1 ]; then
        units+=("${i} -")
    fi
done

unit_label() { # <name> <shard spec or ->
    if [ "$2" = "-" ]; then printf '%s' "$1"; else printf '%s [shard %s]' "$1" "$2"; fi
}
unit_log() { # <name> <shard spec or ->
    if [ "$2" = "-" ]; then
        printf '%s/%s.log' "${log_dir}" "$1"
    else
        printf '%s/%s.shard-%s-of-%s.log' "${log_dir}" "$1" "${2%/*}" "${2#*/}"
    fi
}

# Runs one unit; its exit code goes to <log>.rc.
run_unit() { # <test index> <shard spec or ->
    local t="${tests[$1]}" name wd log rc=0 started="${SECONDS}"
    local -a cmd
    name="$(jq -r '.name' <<<"${t}")"
    wd="$(jq -r '.wd' <<<"${t}")"
    mapfile -t cmd < <(jq -r '.command[]' <<<"${t}")
    log="$(unit_log "${name}" "$2")"
    (
        # errexit is off in a subshell tested by ||, so a failed cd must exit here.
        [ -z "${wd}" ] || cd "${wd}" || exit 97
        if [ "$2" != "-" ]; then export AZ_IOT_TEST_SHARD="$2"; fi
        "${valgrind}" --tool="${tool}" --error-exitcode=42 -s --num-callers=40 \
            "${tool_opts[@]}" --suppressions="${supp}" "${cmd[@]}"
    ) >"${log}" 2>&1 || rc=$?
    echo "${rc}" >"${log}.rc"
    echo "${tool}: $(unit_label "${name}" "$2"): finished in $((SECONDS - started))s (exit ${rc})"
}

echo "${tool}: ${#units[@]} process(es) for ${#tests[@]} test(s), ${jobs} at a time"
running=0
for unit in "${units[@]}"; do
    if [ "${running}" -ge "${jobs}" ]; then
        wait -n || true
        running=$((running - 1))
    fi
    read -r index spec <<<"${unit}"
    run_unit "${index}" "${spec}" &
    running=$((running + 1))
done
wait

status=0
for unit in "${units[@]}"; do
    read -r i spec <<<"${unit}"
    name="$(jq -r '.name' <<<"${tests[i]}")"
    wd="$(jq -r '.wd' <<<"${tests[i]}")"
    label="$(unit_label "${name}" "${spec}")"
    log="$(unit_log "${name}" "${spec}")"
    rc="$(cat "${log}.rc" 2>/dev/null || echo 99)"

    echo "::group::${tool}: ${label}"
    cat "${log}" 2>/dev/null || true
    echo "::endgroup::"

    if [ "${rc}" -eq 97 ] && ! grep -q "ERROR SUMMARY:" "${log}"; then
        annotate "${tool}: ${label}: cannot enter working directory ${wd}"
        status=1
    elif ! grep -q "ERROR SUMMARY:" "${log}" 2>/dev/null; then
        annotate "${tool}: ${label} did not run to completion (exit ${rc})"
        status=1
    elif [ "${rc}" -eq 42 ]; then
        annotate "${tool}: ${label} reported a threading defect:
$(grep -E '^==[0-9]+==' "${log}" | sed '1,/Command:/d' | head -40)"
        status=1
    elif [ "${rc}" -ne 0 ]; then
        annotate "${tool}: ${label} failed (exit ${rc})"
        status=1
    fi
    echo "${tool}: ${label}: $(grep -h 'ERROR SUMMARY:' "${log}" | tail -n 1 | sed -E 's/^==[0-9]+== //')"
    grep -h 'used_suppression:' "${log}" | sed -E 's/^--[0-9]+-- +/  /' || true
done
exit "${status}"
