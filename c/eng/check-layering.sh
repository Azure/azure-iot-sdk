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
    "mqttv3 source must not reference mqttv5" \
    "mqttv5_" \
    "${root_dir}/src/mqttv3"
report_pattern \
    "mqttv5 source must not reference mqttv3" \
    "mqttv3_" \
    "${root_dir}/src/mqttv5"
report_pattern \
    "mqttv3 public headers must not include mqttv5 headers" \
    "#[[:space:]]*include[^[:cntrl:]]*mqttv5/" \
    "${root_dir}/inc/azure/iot/mqttv3"
report_pattern \
    "mqttv5 public headers must not include mqttv3 headers" \
    "#[[:space:]]*include[^[:cntrl:]]*mqttv3/" \
    "${root_dir}/inc/azure/iot/mqttv5"
report_pattern \
    "core must publish profiles as data, not reference generation clients" \
    "mqttv[35]_" \
    "${root_dir}/src/core"
report_pattern \
    "mqttv3 public headers expose an mqttv5-only construct" \
    "az_iot_mqtt_user_property|correlation_data|twin_push|method_probe|ready_handshake" \
    "${root_dir}/inc/azure/iot/mqttv3"
report_pattern \
    "mqttv5 public headers expose an mqttv3-only construct" \
    "az_iot_file_upload_http_transport|sas_uri|property_bag" \
    "${root_dir}/inc/azure/iot/mqttv5"
report_pattern \
    "generation symbol names must not contain classic/next/aeg/flavor" \
    "az_iot_mqttv[35]_[[:alnum:]_]*(classic|next|aeg|flavor)|AZ_IOT_MQTTV[35]_[[:alnum:]_]*(CLASSIC|NEXT|AEG|FLAVOR)" \
    "${root_dir}/src/mqttv3" \
    "${root_dir}/src/mqttv5" \
    "${root_dir}/inc/azure/iot/mqttv3" \
    "${root_dir}/inc/azure/iot/mqttv5"

# Teardown naming. A caller-allocated struct is torn down by _deinit(); only
# something _create() allocated and returned is _destroy()ed. The allow-list is
# exactly the MQTT factories and the e2e service helper. The az_iot_mqtt_iface
# vtable hook is a struct member, not an az_iot_* function, so it never matches.
destroy_hits="$(grep -rnoE '\baz_iot_[a-z0-9_]*_destroy\b' \
    --include='*.c' --include='*.h' \
    "${root_dir}/inc" "${root_dir}/src" "${root_dir}/adapters" \
    "${root_dir}/tests" "${root_dir}/samples" 2>/dev/null \
    | grep -vxE '.*:az_iot_(mock_mqtt|paho|rust_mqtt|esp_mqtt|mymqtt)_factory_destroy|.*:az_iot_e2e_service_destroy' \
    || true)"
if [ -n "${destroy_hits}" ]; then
    if [ "${violations}" -eq 0 ]; then
        echo "Generation layering violations found:"
        echo
    fi
    violations=$((violations + 1))
    echo "  caller-allocated teardown must be _deinit(), not _destroy()"
    while IFS= read -r hit; do
        [ -n "${hit}" ] && echo "    ${hit#${root_dir}/}"
    done <<< "${destroy_hits}"
    echo
fi

if [ "${violations}" -gt 0 ]; then
    echo "${violations} generation layering rule(s) violated."
    exit 1
fi

echo "Generation layering is clean."
