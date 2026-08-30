#!/usr/bin/env bash
# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.
#
# Provision a SoftHSM2 token holding a device private key, so the PKCS#11
# custody tests (design decision D8) have something to sign with.
#
# It imports an EXISTING device key rather than generating one, because the key
# has to match a certificate an enrollment already trusts: CI generates the
# X.509 material when it provisions DPS, and this script moves the key into a
# token and then deletes the file copy. From that point the key exists only
# inside the token, which is the property under test.
#
# Usage:
#   eng/setup-softhsm.sh <device-key.pem> [--keep-key-file]
#
# It prints shell exports on stdout; everything else goes to stderr, so:
#   eval "$(c/eng/setup-softhsm.sh /tmp/device-key.pem)"
#
# Exports:
#   SOFTHSM2_CONF             the generated token configuration
#   PKCS11_PROVIDER_MODULE    the SoftHSM2 PKCS#11 module
#   AZ_IOT_CLIENT_KEY_URI     RFC 7512 URI of the imported key
#   AZ_IOT_CRYPTO_ENGINE_ID   "pkcs11"
#   AZ_IOT_TEST_PKCS11_KEY_URI  same URI, for the unit-level custody suite
#
# Requirements: softhsm2-util, pkcs11-tool (opensc), openssl 3.x and an OpenSSL
# pkcs11 provider (the `pkcs11-provider` package). Each is checked, and a
# missing one is a hard error -- a token that silently is not there would turn
# the custody tests into tests of nothing.

set -euo pipefail

key_file="${1:-}"
keep_key_file="${2:-}"

if [ -z "${key_file}" ] || [ ! -f "${key_file}" ]; then
    echo "usage: $0 <device-key.pem> [--keep-key-file]" >&2
    exit 2
fi

for tool in softhsm2-util pkcs11-tool openssl; do
    command -v "${tool}" >/dev/null 2>&1 || {
        echo "setup-softhsm: '${tool}' not found. Install softhsm2 and opensc." >&2
        exit 1
    }
done

TOKEN_LABEL="${AZ_IOT_PKCS11_TOKEN_LABEL:-aziot}"
KEY_LABEL="${AZ_IOT_PKCS11_KEY_LABEL:-device-key}"
PIN="${AZ_IOT_PKCS11_PIN:-1234}"

work_dir="$(mktemp -d)"
token_dir="${work_dir}/tokens"
mkdir -p "${token_dir}"

conf="${work_dir}/softhsm2.conf"
cat > "${conf}" <<EOF
directories.tokendir = ${token_dir}
objectstore.backend = file
log.level = ERROR
EOF
export SOFTHSM2_CONF="${conf}"

# The module path differs across distributions; ask the loader rather than
# guessing, and fail loudly when it is not there.
module=""
for candidate in \
    /usr/lib/softhsm/libsofthsm2.so \
    /usr/lib64/softhsm/libsofthsm2.so \
    /usr/lib/*/softhsm/libsofthsm2.so \
    "${HOME}"/.local/usr/lib/softhsm/libsofthsm2.so; do
    if [ -f "${candidate}" ]; then
        module="${candidate}"
        break
    fi
done
if [ -z "${module}" ]; then
    echo "setup-softhsm: libsofthsm2.so not found." >&2
    exit 1
fi

echo "setup-softhsm: initializing token '${TOKEN_LABEL}'" >&2
softhsm2-util --module "${module}" --init-token --free \
    --label "${TOKEN_LABEL}" --pin "${PIN}" --so-pin "${PIN}" >&2

# The object is written with pkcs11-tool rather than softhsm2-util --import,
# which handles only a subset of key types.
der="${work_dir}/device-key.der"
openssl pkcs8 -topk8 -nocrypt -in "${key_file}" -outform DER -out "${der}"

echo "setup-softhsm: importing the device key as '${KEY_LABEL}'" >&2
pkcs11-tool --module "${module}" --token-label "${TOKEN_LABEL}" \
    --login --pin "${PIN}" --write-object "${der}" \
    --type privkey --label "${KEY_LABEL}" --id 01 >&2
rm -f "${der}"

# Prove the key is really in the token before anything depends on it.
pkcs11-tool --module "${module}" --token-label "${TOKEN_LABEL}" \
    --login --pin "${PIN}" --list-objects >&2

if [ "${keep_key_file}" != "--keep-key-file" ]; then
    # The point of the exercise: from here the key exists only in the token.
    rm -f "${key_file}"
    echo "setup-softhsm: removed the on-disk copy of the key" >&2
fi

uri="pkcs11:token=${TOKEN_LABEL};object=${KEY_LABEL};type=private?pin-value=${PIN}"

cat <<EOF
export SOFTHSM2_CONF='${conf}'
export PKCS11_PROVIDER_MODULE='${module}'
export AZ_IOT_CLIENT_KEY_URI='${uri}'
export AZ_IOT_CRYPTO_ENGINE_ID='pkcs11'
export AZ_IOT_TEST_PKCS11_KEY_URI='${uri}'
EOF
