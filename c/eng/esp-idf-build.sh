#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Builds one ESP-IDF sample for one target and profile, out of tree. Fails on
# any compiler warning. Requires an ESP-IDF environment (export.sh sourced).
#
#   eng/esp-idf-build.sh <sample-dir> <target> <profile> [build-root]
#
# profile: default             sdkconfig.defaults
#          secure              + sdkconfig.secure
#          secure_<overlay>    + sdkconfig.secure + sdkconfig.secure_<overlay>
# Secure profiles are signed with a key generated for this build only: the
# images are build checks and must never be flashed.
set -euo pipefail

if [ $# -lt 3 ]; then
  echo "usage: $0 <sample-dir> <target> <profile> [build-root]" >&2
  exit 2
fi

sample=$(cd "$1" && pwd)
target=$2
profile=$3
root=${4:-${RUNNER_TEMP:-/tmp}/esp-idf-build}
# Name from the sample's last two path components, e.g. software_update-esp32.
sample_name="$(basename "$(dirname "$sample")")-$(basename "$sample")"
name="${sample_name}-${target}-${profile}"
work="${root}/${name}"
# idf.py set-target cleans the build directory and refuses one that holds
# anything but a CMake build, so generated inputs and the log live beside it.
build="${work}/build"

rm -rf "$work"
mkdir -p "$work"

defaults="${sample}/sdkconfig.defaults"
case "$profile" in
  default) ;;
  secure | secure_*)
    key="${work}/secure_boot_signing_key.pem"
    # ESP32 below chip revision v3.0 has Secure Boot v1 only (ECDSA key).
    if [ "$profile" = secure_esp32_legacy ]; then
      espsecure generate-signing-key --version 1 "$key" > /dev/null
    else
      espsecure generate-signing-key --version 2 --scheme rsa3072 "$key" > /dev/null
    fi
    printf 'CONFIG_SECURE_BOOT_SIGNING_KEY="%s"\n' "$key" > "${work}/signing_key.cfg"
    defaults="${defaults};${sample}/sdkconfig.secure"
    if [ "$profile" != secure ]; then
      defaults="${defaults};${sample}/sdkconfig.${profile}"
    fi
    defaults="${defaults};${work}/signing_key.cfg"
    ;;
  *)
    echo "unknown profile: $profile" >&2
    exit 2
    ;;
esac

log="${work}/build.log"
echo "Building ${sample_name} for ${target} (${profile}) in ${build}"
status=0
idf.py -C "$sample" -B "$build" -D SDKCONFIG="${work}/sdkconfig" \
  -D "SDKCONFIG_DEFAULTS=${defaults}" set-target "$target" build > "$log" 2>&1 || status=$?

if [ "$status" -ne 0 ]; then
  tail -n 100 "$log"
  echo "build failed (exit ${status}); full log: ${log}" >&2
  exit "$status"
fi

grep -E 'binary size' "$log" || true
if grep -q ': warning: ' "$log"; then
  grep ': warning: ' "$log" | sort -u
  echo "build produced compiler warnings" >&2
  exit 1
fi
