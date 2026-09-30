#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Configure, build (warnings are errors) and test one TLS backend.
#
#   eng/ci/build-and-test.sh <openssl|mbedtls|none> [profile]
#
# profile: debug (default), asan (ASan + UBSan), hardened (release with
# fortify, stack protector, CET, full RELRO; binaries are checked afterwards).
# Environment: CC, MBEDTLS_PREFIX (for mbedtls), CMAKE_ARGS (extra, e.g. a
# local cmocka via -DFETCHCONTENT_SOURCE_DIR_CMOCKA=...). E2E tests need
# eng/ci/start-broker.sh first.
set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: ${0##*/} <openssl|mbedtls|none> [debug|asan|hardened]" >&2; exit 1; }
tls="$1"
profile="${2:-debug}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="${root}/build/${tls}-${profile}"

build_type=Debug
c_flags=""
link_flags=""
case "${profile}" in
  debug) ;;
  asan)
    c_flags="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
    link_flags="-fsanitize=address,undefined"
    ;;
  hardened)
    build_type=Release
    c_flags="-O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fcf-protection -fPIE"
    link_flags="-pie -Wl,-z,relro,-z,now,-z,noexecstack"
    ;;
  *) echo "unknown profile ${profile}" >&2; exit 1 ;;
esac

args=(
  -S "${root}" -B "${build}" -G Ninja
  -DCMAKE_BUILD_TYPE="${build_type}"
  -DAZ_MQTT_BUILD_TESTS=ON
  -DAZ_MQTT_BUILD_SAMPLES=ON
  -DAZ_MQTT_TLS_BACKEND="${tls}"
  -DAZ_MQTT_WARNINGS_AS_ERRORS=ON
  -DCMAKE_C_FLAGS="${c_flags}"
  -DCMAKE_EXE_LINKER_FLAGS="${link_flags}"
)
if [ "${tls}" = mbedtls ]; then
  args+=(-DCMAKE_PREFIX_PATH="${MBEDTLS_PREFIX:?set MBEDTLS_PREFIX}")
fi
# shellcheck disable=SC2206
args+=(${CMAKE_ARGS:-})

cmake "${args[@]}"
cmake --build "${build}"

# Link isolation: az_mqtt_core references no version-specific code, and a
# program linked to az_mqttv3 or az_mqttv5 alone contains none of the other.
status=0
core="$(find "${build}" -path "${build}/_deps" -prune -o -name 'libaz_mqtt_core.a' -print -quit)"
[ -n "${core}" ] || { echo "libaz_mqtt_core.a not found" >&2; exit 1; }
if grep -E ' az_mqtt[35]_' <<<"$(nm "${core}")"; then
  echo "${core}: references version-specific symbols" >&2; status=1
fi
for v in 3 5; do
  other=$((8 - v))
  exe="${build}/samples/az_mqtt${v}_sample_connect"
  [ -f "${exe}" ] || continue
  syms="$(nm "${exe}")"
  grep -q " az_mqtt${v}_client_connect" <<<"${syms}" || { echo "${exe}: no mqttv${v} client" >&2; status=1; }
  if grep -E " az_mqtt${other}_" <<<"${syms}"; then
    echo "${exe}: contains mqttv${other} symbols" >&2; status=1
  fi
done
[ "${status}" -eq 0 ] || exit 1
echo "Link isolation checks passed"

if [ "${profile}" = hardened ]; then
  # Every executable we link must be PIE with full RELRO and a non-executable stack.
  status=0
  while IFS= read -r exe; do
    hdr="$(readelf -h -l -d "${exe}")"
    for need in "Type: *DYN" "GNU_RELRO" "BIND_NOW" ; do
      grep -Eq "${need}" <<<"${hdr}" || { echo "${exe}: missing ${need}" >&2; status=1; }
    done
    if grep -A1 GNU_STACK <<<"${hdr}" | grep -q RWE; then
      echo "${exe}: executable stack" >&2; status=1
    fi
  done < <(find "${build}" -path "${build}/_deps" -prune -o -type f -perm -u+x -name "az_mqtt*" -print)
  [ "${status}" -eq 0 ] || exit 1
  echo "Hardening checks passed"
fi

log="${build}/ctest.log"
set +e
ctest --test-dir "${build}" --output-on-failure --timeout 300 2>&1 | tee "${log}"
rc=${PIPESTATUS[0]}
set -e
# Summary as a GitHub annotation (harmless elsewhere).
echo "::notice title=${tls} ${profile}::$(grep -E 'tests passed' "${log}" || echo 'no ctest summary')"
exit "${rc}"
