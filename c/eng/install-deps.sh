#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Build and install azure-sdk-for-c and Eclipse Paho C (static, with OpenSSL) at
# the commits pinned in cgmanifest.json, for a build that uses installed
# dependencies (AZ_IOT_USE_INSTALLED_AZURE_SDK_FOR_C, AZ_IOT_USE_INSTALLED_PAHO).
#
#   eng/install-deps.sh <prefix>
#
# Then configure with -DCMAKE_PREFIX_PATH=<prefix>. azure-sdk-for-c has no
# install rules: its headers go to <prefix>/include and its libraries to
# <prefix>/lib, the layout cmake/Findazure-sdk-for-c.cmake searches.

set -euo pipefail

[ "$#" -eq 1 ] || { echo "usage: ${0##*/} <prefix>" 1>&2; exit 1; }
prefix="$1"
root_dir="$(cd "$(dirname "$0")/.." && pwd)"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

# clone <name> <url> <tag>: shallow clone of <tag>, checked against cgmanifest.json.
clone() {
    local name="$1" url="$2" tag="$3" expected actual
    expected="$(jq -r --arg u "${url}" \
        '.registrations[].component.git | select(.repositoryUrl == $u) | .commitHash' \
        "${root_dir}/cgmanifest.json")"
    git -c advice.detachedHead=false clone -q --depth 1 --branch "${tag}" "${url}" "${work}/${name}"
    actual="$(git -C "${work}/${name}" rev-parse HEAD)"
    if [ "${actual}" != "${expected}" ]; then
        echo "${url} ${tag} is ${actual}, cgmanifest.json pins '${expected}'" 1>&2
        exit 1
    fi
}

clone azure-sdk-for-c https://github.com/Azure/azure-sdk-for-c 1.5.0
clone paho https://github.com/eclipse/paho.mqtt.c v1.3.13

# Without a toolchain file, azure-sdk-for-c would clone vcpkg.
AZURE_SDK_DISABLE_AUTO_VCPKG=1 cmake -S "${work}/azure-sdk-for-c" -B "${work}/azure-sdk-for-c-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DAZ_PLATFORM_IMPL=POSIX \
    -DUNIT_TESTING=OFF \
    -DTRANSPORT_CURL=OFF \
    -DTRANSPORT_PAHO=OFF \
    -DWARNINGS_AS_ERRORS=OFF \
    -DAZ_SDK_C_NO_SAMPLES=ON
cmake --build "${work}/azure-sdk-for-c-build" --parallel \
    --target az_core az_iot_common az_iot_hub az_iot_provisioning az_posix
mkdir -p "${prefix}/include" "${prefix}/lib"
cp -R "${work}/azure-sdk-for-c/sdk/inc/." "${prefix}/include/"
find "${work}/azure-sdk-for-c-build" -name 'libaz_*.a' -exec cp {} "${prefix}/lib/" \;

# CMAKE_POLICY_VERSION_MINIMUM: Paho 1.3.13 declares cmake_minimum_required < 3.5.
cmake -S "${work}/paho" -B "${work}/paho-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DPAHO_BUILD_STATIC=TRUE \
    -DPAHO_BUILD_SHARED=FALSE \
    -DPAHO_WITH_SSL=TRUE \
    -DPAHO_HIGH_PERFORMANCE=TRUE \
    -DPAHO_BUILD_SAMPLES=FALSE \
    -DPAHO_BUILD_DOCUMENTATION=FALSE \
    -DPAHO_ENABLE_TESTING=FALSE \
    -DPAHO_WITH_LIBUUID=FALSE \
    ${OPENSSL_ROOT_DIR:+-DOPENSSL_ROOT_DIR="${OPENSSL_ROOT_DIR}"}
cmake --build "${work}/paho-build" --parallel
cmake --install "${work}/paho-build"
