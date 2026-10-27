#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Build and install azure-sdk-for-c and Eclipse Paho C (static, with OpenSSL) at
# the commits pinned in cgmanifest.json, for a build that uses installed
# dependencies (AZ_IOT_USE_INSTALLED_AZURE_SDK_FOR_C, AZ_IOT_USE_INSTALLED_PAHO).
#
#   eng/install-deps.sh <prefix> [azure-sdk-for-c] [paho]   (default: both)
#
# Then configure with -DCMAKE_PREFIX_PATH=<prefix>. azure-sdk-for-c has no
# install rules: its headers go to <prefix>/include and its libraries to
# <prefix>/lib, the layout cmake/Findazure-sdk-for-c.cmake searches. On Windows
# (MSVC, Ninja) it is built Release into <prefix>/lib and Debug into
# <prefix>/debug/lib, with its static C runtime (/MT, /MTd).
#
# azure-sdk-for-c is built with the software updates manifest limits, which must
# match the SDK configuration: AZ_IOT_SU_MAX_STEP_COUNT, AZ_IOT_SU_MAX_FILE_COUNT
# and AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP from the environment, else the SDK
# defaults (10, 10, and the file count).

set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: ${0##*/} <prefix> [azure-sdk-for-c] [paho]" 1>&2; exit 1; }
prefix="$1"
shift
components=" ${*:-azure-sdk-for-c paho} "
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) windows=1 ;;
    *) windows=0 ;;
esac
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

# build_azure_sdk_for_c <build type> <platform> <lib dir>
build_azure_sdk_for_c() {
    local type="$1" platform="$2" libdir="$3" build="${work}/azure-sdk-for-c-build-$1" generator=()
    if [ "${windows}" = 1 ]; then generator=(-G Ninja); fi
    local steps="${AZ_IOT_SU_MAX_STEP_COUNT:-10}" files="${AZ_IOT_SU_MAX_FILE_COUNT:-10}"
    local files_per_step="${AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP:-${files}}"
    # Without a toolchain file, azure-sdk-for-c would clone vcpkg. CFLAGS adds to
    # the platform's default flags.
    CFLAGS="${CFLAGS:-} -D_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS=${steps} \
-D_az_IOT_ADU_CLIENT_MAX_TOTAL_FILE_COUNT=${files} \
-D_az_IOT_ADU_CLIENT_MAX_FILE_COUNT_PER_STEP=${files_per_step}" \
    AZURE_SDK_DISABLE_AUTO_VCPKG=1 cmake -S "${work}/azure-sdk-for-c" -B "${build}" "${generator[@]}" \
        -DCMAKE_BUILD_TYPE="${type}" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DAZ_PLATFORM_IMPL="${platform}" \
        -DUNIT_TESTING=OFF \
        -DTRANSPORT_CURL=OFF \
        -DTRANSPORT_PAHO=OFF \
        -DWARNINGS_AS_ERRORS=OFF \
        -DAZ_SDK_C_NO_SAMPLES=ON
    cmake --build "${build}" --parallel \
        --target az_core az_iot_common az_iot_hub az_iot_provisioning "az_$(echo "${platform}" | tr '[:upper:]' '[:lower:]')"
    mkdir -p "${libdir}"
    find "${build}" \( -name 'libaz_*.a' -o -name 'az_*.lib' \) -exec cp {} "${libdir}/" \;
}

if [[ "${components}" == *" azure-sdk-for-c "* ]]; then
    clone azure-sdk-for-c https://github.com/Azure/azure-sdk-for-c 1.5.0
    mkdir -p "${prefix}/include"
    cp -R "${work}/azure-sdk-for-c/sdk/inc/." "${prefix}/include/"
    if [ "${windows}" = 1 ]; then
        build_azure_sdk_for_c Release WIN32 "${prefix}/lib"
        build_azure_sdk_for_c Debug WIN32 "${prefix}/debug/lib"
    else
        build_azure_sdk_for_c Release POSIX "${prefix}/lib"
    fi
fi

if [[ "${components}" == *" paho "* ]]; then
    clone paho https://github.com/eclipse/paho.mqtt.c v1.3.16
    # CMAKE_POLICY_VERSION_MINIMUM: Paho before 1.3.14 declares cmake_minimum_required < 3.5.
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
fi
