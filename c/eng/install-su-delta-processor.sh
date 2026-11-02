#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Download, verify and extract the pinned delta processor (libadudiffapi.so, from the prebuilt
# Debian package of github.com/Azure/iot-hub-device-update-delta) for the software updates
# microsoft/delta:1 download handler tests. No root needed: dpkg-deb -x only.
#
#   eng/install-su-delta-processor.sh <directory>   # prints the library path
#   eng/install-su-delta-processor.sh --sha256      # prints the package SHA-256 (cache key)
#
# A package already in <directory> is reused when its SHA-256 matches, so CI can cache
# <directory>/<package> keyed on the SHA-256. x86_64 Linux only; the library needs glibc 2.34+.

set -euo pipefail

commit="f31e98446d2527531e1222e8ebd43b6ccd782605" # "Preview 3.0.0" (Git LFS objects)
package="ms-adu_diffs_3.0.0_x86_64.deb"
sha256="fa775332b7b24cbabbb9716fd8dedabad7b111c131b8c7747bf69ed155cbd4c6"
url="https://media.githubusercontent.com/media/Azure/iot-hub-device-update-delta/${commit}/preview/3.0.0/${package}"

usage() {
    echo "usage: ${0##*/} <directory> | --sha256" 1>&2
    exit 1
}

[ "$#" -eq 1 ] || usage
if [ "$1" = "--sha256" ]; then
    echo "${sha256}"
    exit 0
fi

directory="$1"
mkdir -p "${directory}"
if ! echo "${sha256}  ${directory}/${package}" | sha256sum -c --status 2>/dev/null; then
    curl -sSfL --retry 3 -o "${directory}/${package}" "${url}"
fi
echo "${sha256}  ${directory}/${package}" | sha256sum -c - 1>&2

rm -rf "${directory}/root"
dpkg-deb -x "${directory}/${package}" "${directory}/root"
library="${directory}/root/usr/lib/libadudiffapi.so"
if [ ! -f "${library}" ]; then
    echo "${package} has no usr/lib/libadudiffapi.so" 1>&2
    exit 1
fi
echo "$(cd "$(dirname "${library}")" && pwd)/$(basename "${library}")"
