#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Build and install a maintained mbedTLS release (crypto + CMake package) for the
# software updates mbedTLS crypto adapter tests.
#
#   eng/install-mbedtls.sh <version> <prefix>
#
# Then configure with -DCMAKE_PREFIX_PATH=<prefix>. Only the versions pinned below are
# accepted; add a line (with the release's published SHA-256) to test another.

set -euo pipefail

usage() {
    echo "usage: ${0##*/} <version> <prefix>" 1>&2
    exit 1
}

[ "$#" -eq 2 ] || usage
version="$1"
prefix="$2"

case "${version}" in
    3.6.7) sha256="a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6" ;;
    4.1.1) sha256="3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c" ;;
    4.2.0) sha256="2bed9d713b4668f76553b097e72b8aa30bc8f112a940d7ae228d524bbde6ffea" ;;
    *) echo "mbedTLS ${version} is not pinned in ${0##*/}" 1>&2; exit 1 ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

archive="mbedtls-${version}.tar.bz2"
curl -sSfL -o "${work}/${archive}" \
    "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${version}/${archive}"
echo "${sha256}  ${work}/${archive}" | sha256sum -c -
tar -xjf "${work}/${archive}" -C "${work}"

cmake -S "${work}/mbedtls-${version}" -B "${work}/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF
cmake --build "${work}/build" --parallel
cmake --install "${work}/build"
