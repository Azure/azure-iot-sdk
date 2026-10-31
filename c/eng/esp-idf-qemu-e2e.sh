#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Runs az_iot_tests_e2e_esp32_qemu: builds the ESP32 IoT Hub sample's qemu firmware for <target>
# with the device's DPS settings, writes its identity (eng/esp-idf-qemu-identity.sh), then builds
# and runs the test, which boots the firmware in QEMU. Requires an ESP-IDF environment with QEMU,
# a host C compiler and OpenSSL headers.
#
#   eng/esp-idf-qemu-e2e.sh <target> <cert.pem> <key.pem> [work-dir]
#
# Environment: E2E_ESP32_DEVICE_ID (the DPS registration id), IOT_DPS_ID_SCOPE, optional
# IOT_DPS_GLOBAL_ENDPOINT, and the e2e service settings (IOTHUB_CONNECTION_STRING,
# IOTHUB_EVENTHUB_CONNECTION_STRING, IOTHUB_EVENTHUB_CONSUMER_GROUP).
set -euo pipefail

if [ $# -lt 3 ]; then
  echo "usage: $0 <target> <cert.pem> <key.pem> [work-dir]" >&2
  exit 2
fi
: "${E2E_ESP32_DEVICE_ID:?}" "${IOT_DPS_ID_SCOPE:?}"
target=$1
cert=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
key=$(cd "$(dirname "$3")" && pwd)/$(basename "$3")
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
work=${4:-${RUNNER_TEMP:-/tmp}/esp32-qemu-e2e}
mkdir -p "$work"
work=$(cd "$work" && pwd)

{
  printf 'CONFIG_HUB_DPS_ID_SCOPE="%s"\n' "$IOT_DPS_ID_SCOPE"
  printf 'CONFIG_HUB_DPS_REGISTRATION_ID="%s"\n' "$E2E_ESP32_DEVICE_ID"
  if [ -n "${IOT_DPS_GLOBAL_ENDPOINT:-}" ]; then
    printf 'CONFIG_HUB_DPS_GLOBAL_ENDPOINT="%s"\n' "$IOT_DPS_GLOBAL_ENDPOINT"
  fi
  echo 'CONFIG_HUB_TELEMETRY_INTERVAL_S=5'
} > "${work}/sdkconfig.e2e"
SDKCONFIG_EXTRA="${work}/sdkconfig.e2e" bash "${here}/esp-idf-build.sh" \
  "${root}/samples/unified/esp32" "$target" qemu "${work}/firmware"
build="${work}/firmware/unified-esp32-${target}-qemu/build"

bash "${here}/esp-idf-qemu-identity.sh" "$build" "$target" "$cert" "$key" "${work}/identity"
efuse=
if [ -f "${work}/identity/qemu_efuse.bin" ]; then
  efuse="QEMU_EFUSE='${work}/identity/qemu_efuse.bin' "
fi
export E2E_ESP32_QEMU_COMMAND="QEMU_FLASH_EXTRA='0xd000 ${work}/identity/esp_secure_cert.bin' \
${efuse}bash '${here}/esp-idf-qemu.sh' '${build}' ${target} --run"

tests="${work}/tests"
cmake -S "$root" -B "$tests" -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug \
  -DAZ_IOT_BUILD_TESTS=ON -DAZ_IOT_BUILD_SAMPLES=OFF -DAZ_IOT_WITH_PAHO=OFF \
  -DAZ_IOT_WITH_AZ_MQTT=ON -DAZ_IOT_BUILD_E2E=ON -DAZ_IOT_BUILD_E2E_ESP32_QEMU=ON > /dev/null
cmake --build "$tests" --target az_iot_tests_e2e_esp32_qemu > "${work}/tests-build.log" 2>&1 \
  || { tail -n 60 "${work}/tests-build.log"; exit 1; }
ctest --test-dir "$tests" -R '^az_iot_tests_e2e_esp32_qemu$' --output-on-failure --no-tests=error
