#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Keeps non-GA features out of main: fails, listing file:line, when tracked C
# content (c/, the C workflows and the C instructions) names one of them. The
# features live on releases/public-preview, which reverts this guard with them.
# Categories:
# - su: software updates (az_iot_su*, AZ_IOT_*SU*, software_update, su_delta,
#   features/su, adapters/su, verify_rs256);
# - mqttv5: the mqttv5 hub clients and MQTT 5 (mqttv5, mqtt_v5, MQTT_V5, any case).
# Word boundaries are explicit: az_iot_subscription_* and AZ_IOT_SUBSCRIPTION_*
# are not software updates. File names are checked too. What may stay is in
# allowed_files and allowed_tokens_* below, each with its reason.
# Dependency-free (git, awk) so it runs in the conventions job before configure.
#
# Usage: check-ga-scope.sh [--only su|mqttv5[,...]]   (default: every category)
#        check-ga-scope.sh --self-test                (fixtures built in a temporary repo)
#        check-ga-scope.sh --root DIR ...             (scan another repository; used by --self-test)

set -euo pipefail

script_path="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
all_categories=(su mqttv5)

# Forbidden content, as POSIX EREs (git grep -E and awk).
forbidden_su=(
    '(az_iot|AZ_IOT)(_[A-Za-z0-9]+)*_(su|SU)([^A-Za-z0-9]|$)'
    '[Ss]oftware[_-][Uu]pdate|SOFTWARE[_-]UPDATE'
    '(features|adapters)/su([^A-Za-z0-9]|$)'
    '(^|[^A-Za-z0-9])(su|SU)[_-](delta|DELTA)'
    'verify_rs256|VERIFY_RS256'
)
forbidden_mqttv5=(
    '[Mm][Qq][Tt][Tt]_?[Vv]5'
)

# Scanned: tracked files only.
# Keep in sync with the ga_scope filter in .github/workflows/ci-c.yml.
scope=(
    c
    ':(glob).github/workflows/ci-c*.yml'
    .github/workflows/codeql-c.yml
    ':(glob).github/instructions/c-*.md'
)

# Files that may name any category, whole.
allowed_files=(
    # This guard: its pattern lists.
    c/eng/check-ga-scope.sh
    # Preview-only workflows. workflow_dispatch needs the file on the default
    # branch, so they stay on main, trigger on releases/public-preview only and
    # do nothing on main.
    .github/workflows/ci-c-e2e-mqttv5.yml
    .github/workflows/ci-c-e2e-mqttv5-adr-namespace.yml
    .github/workflows/ci-c-e2e-adu.yml
    # Dispatches the workflows above on releases/public-preview.
    .github/workflows/ci-c-public-preview-nightly.yml
    # Vendored MQTT library: keeps its own MQTT 5 client, which the SDK builds
    # without (c/adapters/az_mqtt/CMakeLists.txt, allowed below).
    c/deps/az_mqtt
)

# Tokens that may stay on a line: "<path ERE><TAB><token ERE>" pairs. A token
# is removed before the line is matched again, so anything else on the line
# still fails.
allowed_tokens_su=()
allowed_tokens_mqttv5=(
    # The DPS connectionProfile value: main parses it to fail closed with
    # AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED; tests and docs quote it. The
    # kept AZ_IOT_CONNECTION_PROFILE_MQTT_V3/_UNKNOWN do not match.
    $'.*\t\\\\?"mqttV5\\\\?"'
    $'.*\t`mqttV5`'
    # References to the preview-only workflow files kept on main (above).
    $'.*\tci-c-e2e-mqttv5[A-Za-z0-9._-]*'
    # Builds the vendored az_mqtt without its MQTT 5 client.
    $'c/adapters/az_mqtt/CMakeLists\\.txt\tset\\(AZ_MQTT_ENABLE_MQTTV5 OFF([^A-Za-z0-9_]|$)'
    # The test that main rejects the mqttV5 profile, and its coverage entry.
    $'c/tests/unit/connection_dps_test\\.c\t(^|[^A-Za-z0-9_])k_assigned_mqtt_v5([^A-Za-z0-9_]|$)'
    $'c/(tests/unit/connection_dps_test\\.c|docs/eng/test-coverage\\.md)\t(^|[^A-Za-z0-9_])dps_mqtt_v5_profile_is_unsupported([^A-Za-z0-9_]|$)'
)

if [ "${1:-}" = "--self-test" ]; then
    work="$(mktemp -d)"
    trap 'rm -rf "${work}"' EXIT
    failures=0
    # expect <pass|fail> <--only value> <file> <content>: one fixture per run.
    expect() {
        local expected="$1" only="$2" file="$3" content="$4" verdict
        rm -rf "${work}/repo"
        git init -q "${work}/repo"
        mkdir -p "$(dirname "${work}/repo/${file}")"
        printf '%s\n' "${content}" >"${work}/repo/${file}"
        git -C "${work}/repo" add -A
        if bash "${script_path}" --root "${work}/repo" --only "${only}" >/dev/null 2>&1; then
            verdict=pass
        else
            verdict=fail
        fi
        if [ "${verdict}" != "${expected}" ]; then
            echo "self-test: [${only}] ${file}: '${content}': expected ${expected}, got ${verdict}"
            failures=$((failures + 1))
        fi
    }
    f=c/src/x.c
    expect fail su "${f}" '#include "azure/iot/az_iot_su.h"'
    expect fail su "${f}" 'az_iot_su_client_init(&client);'
    expect fail su "${f}" 'return AZ_IOT_SU_RESULT_SUCCESS;'
    expect fail su "${f}" 'AZ_IOT_LOG_INFO(AZ_IOT_LOG_COMPONENT_SU, "x");'
    expect fail su "${f}" 'cmake -DAZ_IOT_WITH_SU_DELTA=ON'
    expect fail su "${f}" 'AZ_IOT_TEST_SU_DELTA_LIBRARY'
    expect fail su "${f}" 'add_subdirectory(samples/software_update)'
    expect fail su "${f}" 'see docs/software-update.md'
    expect fail su "${f}" 'add_subdirectory(features/su)'
    expect fail su "${f}" 'c/adapters/su/esp32'
    expect fail su "${f}" 'SU_DELTA_LIBRARY: x'
    expect fail su "${f}" 'test_su_delta_apply'
    expect fail su "${f}" 'verify_rs256(&key);'
    expect fail su c/src/features/su/client.c 'int x;'
    expect fail su c/tests/unit/data/su_delta/a.bin 'x'
    expect fail su .github/workflows/ci-c.yml '- c/src/features/su/**'
    expect fail su .github/workflows/codeql-c.yml 'az_iot_su_client'
    expect pass su "${f}" 'az_iot_subscription_failed_callback cb;'
    expect pass su "${f}" 'AZ_IOT_SUBSCRIPTION_FAILS_SELF AZ_IOT_ERR_SUBSCRIPTION_REFUSED'
    expect pass su "${f}" 'AZ_IOT_DPS_PHASE_SUBSCRIBING AZ_IOT_MOCK_CALL_SUBSCRIBE'
    expect pass su "${f}" 'az_iot_mqtt_suback features/subscriptions issue_delta'
    expect pass su "${f}" 'mqttv5 is another category'
    expect pass su .github/workflows/ci-c-e2e-adu.yml 'az_iot_su_client'
    expect pass su other/x.c 'az_iot_su_client'
    expect fail mqttv5 "${f}" 'az_iot_mqttv5_twin_client_init(&c);'
    expect fail mqttv5 "${f}" 'AZ_IOT_MQTTV5_H'
    expect fail mqttv5 "${f}" 'AZ_IOT_CONNECTION_PROFILE_MQTT_V5'
    expect fail mqttv5 "${f}" 'static const char k_mqtt_v5[] = "x";'
    expect fail mqttv5 "${f}" 'properties.mqttV5Settings'
    expect fail mqttv5 "${f}" '"mqttV5" is quoted but mqttv5 is not'
    expect fail mqttv5 c/samples/mqttv5/telemetry/main.c 'int x;'
    expect fail mqttv5 .github/workflows/ci-c-yocto.yml 'for c in core mqttv3 mqttv5'
    expect fail mqttv5 .github/instructions/c-library.instructions.md 'mqttv5'
    expect pass mqttv5 "${f}" 'else if (strcmp(raw, "mqttV5") == 0)'
    expect pass mqttv5 "${f}" '"\"connectionProfile\":\"mqttV5\","'
    # shellcheck disable=SC2016 # backticks are fixture text
    expect pass mqttv5 c/docs/x.md 'DPS returns `mqttV5`: the client fails closed.'
    expect pass mqttv5 c/docs/x.md 'see ci-c-e2e-mqttv5.yml and ci-c-e2e-mqttv5-gate'
    expect pass mqttv5 "${f}" 'AZ_IOT_CONNECTION_PROFILE_MQTT_V3 AZ_IOT_CONNECTION_PROFILE_UNKNOWN mqttv3'
    expect pass mqttv5 "${f}" 'az_iot_su_client is another category'
    expect pass mqttv5 .github/workflows/ci-c-e2e-mqttv5.yml 'az_iot_mqttv5'
    expect pass mqttv5 .github/workflows/ci-c-public-preview-nightly.yml 'ci-c-e2e-adu.yml mqttv5'
    expect pass mqttv5 c/eng/check-ga-scope.sh 'mqttv5'
    expect pass mqttv5 c/deps/az_mqtt/src/az_mqtt5_client.c 'az_mqttv5 AZ_MQTT_ENABLE_MQTTV5'
    expect pass mqttv5 c/deps/az_mqtt/mqttv5/x.c 'int x;'
    expect fail mqttv5 c/deps/other/x.c 'az_mqttv5'
    expect pass mqttv5 c/adapters/az_mqtt/CMakeLists.txt 'set(AZ_MQTT_ENABLE_MQTTV5 OFF CACHE BOOL "" FORCE)'
    expect fail mqttv5 c/adapters/az_mqtt/CMakeLists.txt 'set(AZ_MQTT_ENABLE_MQTTV5 ON CACHE BOOL "" FORCE)'
    expect fail mqttv5 c/adapters/az_mqtt/CMakeLists.txt 'set(AZ_MQTT_ENABLE_MQTTV5 OFF) # az_mqttv5'
    expect fail mqttv5 c/CMakeLists.txt 'set(AZ_MQTT_ENABLE_MQTTV5 OFF CACHE BOOL "" FORCE)'
    expect pass mqttv5 c/tests/unit/connection_dps_test.c 'profile_assign(&pf, k_assigned_mqtt_v5);'
    expect pass mqttv5 c/tests/unit/connection_dps_test.c 'cmocka_unit_test(dps_mqtt_v5_profile_is_unsupported),'
    expect pass mqttv5 c/docs/eng/test-coverage.md '| [dps_mqtt_v5_profile_is_unsupported](x.c#L1) |'
    expect fail mqttv5 c/tests/unit/connection_dps_test.c 'static void dps_mqtt_v5_profile_is_unsupported_too(void);'
    expect fail mqttv5 c/tests/unit/connection_dps_test.c 'k_assigned_mqtt_v5 az_iot_mqttv5'
    expect fail mqttv5 c/tests/unit/connection_twin_test.c 'k_assigned_mqtt_v5'
    expect fail mqttv5 c/docs/eng/test-coverage.md 'k_assigned_mqtt_v5'
    expect fail su,mqttv5 "${f}" 'az_iot_mqttv5'
    [ "${failures}" -eq 0 ] || exit 1
    echo "self-test: every fixture gave the expected verdict."
    exit 0
fi

root=""
categories=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --only)
            [ "$#" -ge 2 ] || { echo "--only needs a category list" >&2; exit 2; }
            IFS=',' read -r -a requested <<<"$2"
            categories+=("${requested[@]}")
            shift 2
            ;;
        --root)
            [ "$#" -ge 2 ] || { echo "--root needs a directory" >&2; exit 2; }
            root="$2"
            shift 2
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done
[ "${#categories[@]}" -gt 0 ] || categories=("${all_categories[@]}")
for category in "${categories[@]}"; do
    [[ " ${all_categories[*]} " == *" ${category} "* ]] ||
        { echo "unknown category '${category}' (known: ${all_categories[*]})" >&2; exit 2; }
done
[ -n "${root}" ] || root="$(git -C "$(dirname "${script_path}")" rev-parse --show-toplevel)"

pathspecs=("${scope[@]}")
for f in "${allowed_files[@]}"; do
    pathspecs+=(":(exclude)${f}")
done

violations=0
for category in "${categories[@]}"; do
    case "${category}" in
        su) patterns=("${forbidden_su[@]}") tokens=("${allowed_tokens_su[@]}") ;;
        mqttv5) patterns=("${forbidden_mqttv5[@]}") tokens=("${allowed_tokens_mqttv5[@]}") ;;
    esac
    grep_args=()
    joined=""
    for p in "${patterns[@]}"; do
        grep_args+=(-e "${p}")
        joined="${joined:+${joined}|}(${p})"
    done

    rc=0
    matches="$(git -C "${root}" grep -nIE "${grep_args[@]}" -- "${pathspecs[@]}")" || rc=$?
    [ "${rc}" -le 1 ] || { echo "git grep failed (${rc})" >&2; exit 2; }
    names="$(git -C "${root}" ls-files -- "${pathspecs[@]}" | grep -E "${joined}" || true)"

    found="$(
        {
            [ -z "${matches}" ] || printf '%s\n' "${matches}"
            [ -z "${names}" ] || printf '%s\n' "${names}" | sed 's/$/:0:/'
        } | GA_FORBIDDEN="${joined}" GA_TOKENS="$(printf '%s\n' "${tokens[@]+"${tokens[@]}"}")" awk '
            BEGIN {
                n = split(ENVIRON["GA_TOKENS"], entries, "\n")
                for (i = 1; i <= n; i++) {
                    tab = index(entries[i], "\t")
                    if (tab == 0) continue
                    token_path[i] = substr(entries[i], 1, tab - 1)
                    token[i] = substr(entries[i], tab + 1)
                }
            }
            {
                first = index($0, ":")
                file = substr($0, 1, first - 1)
                rest = substr($0, first + 1)
                second = index(rest, ":")
                line = substr(rest, 1, second - 1)
                text = substr(rest, second + 1)
                checked = (line == 0) ? file : text
                for (i in token) {
                    if (file ~ ("^(" token_path[i] ")$")) gsub(token[i], "", checked)
                }
                if (checked !~ ENVIRON["GA_FORBIDDEN"]) next
                sub(/^[ \t]+/, "", text)
                if (length(text) > 160) text = substr(text, 1, 157) "..."
                if (line == 0) print file ": file name"
                else print file ":" line ": " text
            }'
    )"
    if [ -n "${found}" ]; then
        count="$(printf '%s\n' "${found}" | wc -l)"
        echo "${category}: ${count} non-GA line(s):"
        printf '%s\n' "${found}" | sed 's/^/  /'
        violations=$((violations + count))
    fi
done

if [ "${violations}" -gt 0 ]; then
    echo "${violations} non-GA line(s) on a GA branch. These features live on releases/public-preview;"
    echo "remove them, or justify an entry in the allowlists of eng/check-ga-scope.sh."
    exit 1
fi
echo "No non-GA content (${categories[*]})."
