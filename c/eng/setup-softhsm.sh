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
#   AZ_IOT_CLIENT_KEY_URI     RFC 7512 URI of the imported key (the PIN is named by
#                             pin-source, never inlined as pin-value)
#   OPENSSL_CONF              an OpenSSL config that loads the pkcs11 provider
#                             with digests disabled (see the note further down)
#   AZ_IOT_CRYPTO_ENGINE_ID   "pkcs11"
#   AZ_IOT_TEST_PKCS11_KEY_URI  same URI, for the unit-level custody suite
#
# Requirements: softhsm2-util, pkcs11-tool (opensc) and openssl, each checked
# below -- a missing one is a hard error, because a token that silently is not
# there would turn the custody tests into tests of nothing.
#
# The OpenSSL pkcs11 PROVIDER is a separate requirement and deliberately not
# checked here: this script only fills a token, and nothing it does needs the
# provider. Build one with setup-pkcs11-provider.sh and verify it with
# `openssl list -providers -provider pkcs11`, which is what CI does.

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

# The public half goes in too, under the same id. A TLS stack has to match the
# key against the certificate it is offering, and it does that through the
# public key the provider exposes for the object -- which a token cannot always
# reconstruct from the private half alone.
pub_der="${work_dir}/device-pub.der"
openssl pkey -in "${key_file}" -pubout -outform DER -out "${pub_der}"
pkcs11-tool --module "${module}" --token-label "${TOKEN_LABEL}" \
    --login --pin "${PIN}" --write-object "${pub_der}" \
    --type pubkey --label "${KEY_LABEL}" --id 01 >&2
rm -f "${pub_der}"

# Prove the key is really in the token before anything depends on it.
pkcs11-tool --module "${module}" --token-label "${TOKEN_LABEL}" \
    --login --pin "${PIN}" --list-objects >&2

if [ "${keep_key_file}" != "--keep-key-file" ]; then
    # The point of the exercise: from here the key exists only in the token.
    rm -f "${key_file}"
    echo "setup-softhsm: removed the on-disk copy of the key" >&2
fi

# The PIN goes in a file the provider reads, named by pin-source -- NOT inline
# as pin-value. The adapter embeds this URI in the key-reference file it hands
# the TLS stack, so an inline PIN would be written to disk by the very code path
# whose purpose is that the credential never lands there; the adapter refuses
# such a URI outright. pin-source names where the PIN lives instead, which keeps
# the reference independently loadable (OpenSSL must log in to the token when it
# resolves the file) without putting the secret in it.
# Where setup-pkcs11-provider.sh put pkcs11.so, if it ran. Naming the module
# explicitly keeps the configuration independent of the ambient module search
# path; when it is unset the provider is found the usual way.
#
# Checked rather than trusted: OPENSSL_MODULES may point at an unrelated modules
# directory, and writing "module = <dir>/pkcs11.so" for a file that is not there
# produces a configuration OpenSSL fails to load -- a worse outcome than simply
# letting the provider be found the usual way.
modules_dir="${OPENSSL_MODULES:-}"
if [ -n "${modules_dir}" ] && [ ! -f "${modules_dir}/pkcs11.so" ]; then
    echo "setup-softhsm: OPENSSL_MODULES='${modules_dir}' holds no pkcs11.so;" \
         "leaving the provider to be located the usual way" >&2
    modules_dir=""
fi

pin_file="${work_dir}/token-pin"
printf '%s' "${PIN}" > "${pin_file}"
chmod 600 "${pin_file}"
uri="pkcs11:token=${TOKEN_LABEL};object=${KEY_LABEL};type=private?pin-source=file:${pin_file}"

# An OpenSSL configuration that loads the pkcs11 provider with DIGESTS DISABLED.
#
# Without this, a TLS 1.2 client-authentication handshake fails outright:
#
#   error:40800054:pkcs11:p11prov_GetOperationState:...:Error returned by
#   C_GetOperationState
#
# The provider offers digest implementations as well as key operations, so once
# it is loaded it can end up servicing the TLS handshake transcript hash. TLS
# 1.2 duplicates that digest context, the provider implements duplication with
# C_GetOperationState, and SoftHSM2 -- like most tokens, by the provider's own
# note in digests.c -- does not support it on a digest session. TLS 1.3 does not
# duplicate the context, which is why only the 1.2 path breaks.
#
# The token never needed to hash anything: only the private key lives there.
# Blocking the digest operation routes hashing back to OpenSSL's default
# provider and leaves signing in the token, which is the whole point.
openssl_cnf="${work_dir}/openssl-pkcs11.cnf"
cat > "${openssl_cnf}" <<CNF
openssl_conf = az_iot_init

[az_iot_init]
providers = az_iot_providers

[az_iot_providers]
default = az_iot_default_sect
pkcs11 = az_iot_pkcs11_sect

[az_iot_default_sect]
activate = 1

[az_iot_pkcs11_sect]
${modules_dir:+module = ${modules_dir}/pkcs11.so}
pkcs11-module-path = ${module}
pkcs11-module-block-operations = digest
# Do not tear the module down when OpenSSL unloads the provider. SoftHSM2
# crashes during that teardown once the provider has been activated from
# configuration, which turns a passing test run into a SEGFAULT at exit. The
# provider documents this quirk for exactly that situation; the cost is memory
# the process was about to release anyway.
pkcs11-module-quirks = no-deinit
# Activation must be here, not left to the adapter's own OSSL_PROVIDER_try_load:
# the settings above apply only to a provider the configuration brings up.
activate = 1
CNF

cat <<EOF
export SOFTHSM2_CONF='${conf}'
export PKCS11_PROVIDER_MODULE='${module}'
export OPENSSL_CONF='${openssl_cnf}'
export AZ_IOT_CLIENT_KEY_URI='${uri}'
export AZ_IOT_CRYPTO_ENGINE_ID='pkcs11'
export AZ_IOT_TEST_PKCS11_KEY_URI='${uri}'
EOF
