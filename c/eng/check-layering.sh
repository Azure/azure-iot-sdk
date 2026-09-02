#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Enforces the generation-client boundaries specified in
# docs/eng/client-separation.md. Keep this dependency-free so it can run in the
# conventions job before the project is configured.

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
violations=0

report_pattern() {
    description="$1"
    pattern="$2"
    shift 2

    hits="$(grep -rniE \
        --include='*.c' --include='*.h' \
        "${pattern}" "$@" 2>/dev/null || true)"
    if [ -z "${hits}" ]; then
        return
    fi

    if [ "${violations}" -eq 0 ]; then
        echo "Generation layering violations found:"
        echo
    fi
    violations=$((violations + 1))
    echo "  ${description}"
    while IFS= read -r hit; do
        [ -n "${hit}" ] && echo "    ${hit#${root_dir}/}"
    done <<< "${hits}"
    echo
}

report_pattern \
    "gen1 source must not reference gen2" \
    "gen2_" \
    "${root_dir}/src/gen1"
report_pattern \
    "gen2 source must not reference gen1" \
    "gen1_" \
    "${root_dir}/src/gen2"
report_pattern \
    "gen1 public headers must not include gen2 headers" \
    "#[[:space:]]*include[^[:cntrl:]]*gen2/" \
    "${root_dir}/inc/azure/iot/gen1"
report_pattern \
    "gen2 public headers must not include gen1 headers" \
    "#[[:space:]]*include[^[:cntrl:]]*gen1/" \
    "${root_dir}/inc/azure/iot/gen2"
report_pattern \
    "core must publish profiles as data, not reference generation clients" \
    "gen[12]_" \
    "${root_dir}/src/core"
report_pattern \
    "gen1 public headers expose a gen2-only construct" \
    "az_iot_mqtt_user_property|correlation_data|twin_push|method_probe|ready_handshake" \
    "${root_dir}/inc/azure/iot/gen1"
report_pattern \
    "gen2 public headers expose a gen1-only construct" \
    "az_iot_file_upload_http_transport|sas_uri|property_bag" \
    "${root_dir}/inc/azure/iot/gen2"
report_pattern \
    "generation symbol names must not contain classic/next/aeg/flavor" \
    "az_iot_gen[12]_[[:alnum:]_]*(classic|next|aeg|flavor)|AZ_IOT_GEN[12]_[[:alnum:]_]*(CLASSIC|NEXT|AEG|FLAVOR)" \
    "${root_dir}/src/gen1" \
    "${root_dir}/src/gen2" \
    "${root_dir}/inc/azure/iot/gen1" \
    "${root_dir}/inc/azure/iot/gen2"

if [ "${violations}" -gt 0 ]; then
    echo "${violations} generation layering rule(s) violated."
    exit 1
fi

echo "Generation layering is clean."
