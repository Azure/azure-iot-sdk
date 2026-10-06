#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Enforces the generation-client boundaries listed in docs/architecture.md
# ("Library boundaries"). Keep this dependency-free so it can run in the
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

# Generation vocabulary, over every tracked text file under the trees listed
# below -- not just C sources, because the PowerShell test-env scripts, CMake,
# READMEs, JSON and .gitignore carry the same names. gen1/gen2/aeg/classic are
# matched anywhere, including inside an identifier, since none of them has a
# legitimate use here. `next` is an ordinary English word, so only the forms
# that pair it with a generation word are banned -- with the separator optional,
# so hub_next, hub-next, next_hub and HubNext all match.
#
# c/docs is deliberately NOT scanned: it records decisions, external RFC paths
# and past defects that legitimately name the old vocabulary.
#
# Two exemptions, both blanked per-occurrence rather than per-line, so a line
# carrying an exempt string AND a banned name is still reported:
#   - the DPS `connectionProfile` wire value, the literal string classic, quoted
#     or backticked;
#   - az-iot-hub-next, the name of a separate external repository.
banned_re='gen1|gen2|aeg|classic|(hub|mock|profile|setup|flavor|assigned|gen)_next|_is_next|hub[-_ ]?next|next[-_ ]?hub|iothub[a-z]*-?next'
banned_hits="$(git -C "${root_dir}/.." ls-files \
        'c/inc/*' 'c/src/*' 'c/adapters/*' 'c/tests/*' 'c/samples/*' 2>/dev/null \
    | grep -vE '\.(pem|der|crt|key|png|jpg|bin)$' \
    | (cd "${root_dir}/.." && xargs -r grep -niIE "${banned_re}" 2>/dev/null) \
    | sed -e 's/\\\{0,1\}"classic\\\{0,1\}"/"@wire@"/g' \
          -e 's/`classic`/`@wire@`/g' \
          -e 's/az-iot-hub-next/az-iot-@extrepo@/g' \
    | grep -iE "${banned_re}" \
    || true)"
if [ -n "${banned_hits}" ]; then
    if [ "${violations}" -eq 0 ]; then
        echo "Generation layering violations found:"
        echo
    fi
    violations=$((violations + 1))
    echo "  generation naming must use mqttv3/mqttv5, not gen1/gen2/classic/next/aeg"
    while IFS= read -r hit; do
        [ -n "${hit}" ] && echo "    ${hit#${root_dir}/}"
    done <<< "${banned_hits}"
    echo
fi
if [ "${violations}" -gt 0 ]; then
    echo "${violations} generation layering rule(s) violated."
    exit 1
fi

echo "Generation layering is clean."
