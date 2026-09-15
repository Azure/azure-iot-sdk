#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Build the OpenSSL 3.x PKCS#11 provider the custody tests sign through, and
# print the export that makes OpenSSL find it.
#
# It is built from source rather than installed from the distribution because
# the version matters and distributions lag: the provider has to register a
# DECODER for its own key-reference PEM, which is what
# SSL_CTX_use_PrivateKey_file -- and therefore Paho -- uses to resolve the file
# back to the key inside the token. Ubuntu 24.04 ships 0.3, which does not, and
# a packaged provider would silently change what the tests prove from one runner
# image to the next.
#
# Usage:
#   eval "$(c/eng/setup-pkcs11-provider.sh [install-dir])"
#
# Exports:
#   OPENSSL_MODULES   directory holding pkcs11.so
#
# Requirements: meson, ninja, pkg-config, a C compiler and OpenSSL 3.x headers.

set -euo pipefail

PKCS11_PROVIDER_TAG="${PKCS11_PROVIDER_TAG:-v0.6}"
PKCS11_PROVIDER_REPO="${PKCS11_PROVIDER_REPO:-https://github.com/latchset/pkcs11-provider.git}"

dest="${1:-${RUNNER_TEMP:-/tmp}/pkcs11-provider}"

for tool in meson ninja git pkg-config; do
    command -v "${tool}" >/dev/null 2>&1 || {
        echo "setup-pkcs11-provider: '${tool}' not found." >&2
        exit 1
    }
done

if [ ! -f "${dest}/builddir/src/pkcs11.so" ]; then
    echo "setup-pkcs11-provider: building ${PKCS11_PROVIDER_TAG}" >&2
    rm -rf "${dest}"
    git clone --quiet --depth 1 --branch "${PKCS11_PROVIDER_TAG}" \
        "${PKCS11_PROVIDER_REPO}" "${dest}" >&2
    meson setup "${dest}/builddir" "${dest}" >&2
    meson compile -C "${dest}/builddir" >&2
fi

if [ ! -f "${dest}/builddir/src/pkcs11.so" ]; then
    echo "setup-pkcs11-provider: build produced no pkcs11.so." >&2
    exit 1
fi

echo "export OPENSSL_MODULES='${dest}/builddir/src'"
