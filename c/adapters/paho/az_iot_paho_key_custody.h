// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Non-extractable key custody for the Paho adapter (design decision D8).
 *
 * ADAPTER-PRIVATE. Not installed, not part of the public API.
 *
 * Paho terminates TLS with OpenSSL and takes the client private key as a FILE
 * PATH (MQTTAsync_SSLOptions::privateKey, loaded with
 * SSL_CTX_use_PrivateKey_file). It exposes no SSL_CTX and no key hook, so the
 * only way to hand it a key that lives inside an HSM / TPM / secure element is
 * to give it a path OpenSSL resolves back to that key rather than to key bytes.
 *
 * That is exactly what an OpenSSL 3.x provider's own PEM form is: pkcs11
 * providers emit a "PKCS#11 PROVIDER URI" block, tpm2 providers emit a
 * "TSS2 PRIVATE KEY" block. Both are references, neither contains private
 * material, and both are decoded by SSL_CTX_use_PrivateKey_file through the
 * provider that produced them.
 *
 * So prepare() does three things:
 *   1. loads the engine/provider named by crypto_engine_id;
 *   2. resolves client_key_uri through it, which PROVES the key is reachable
 *      and signable before a socket exists (the alternative is an unexplained
 *      handshake failure seconds later);
 *   3. re-encodes the resolved key and writes the result to a private
 *      temporary file, whose path Paho then loads.
 *
 * Step 3 refuses to write anything that turns out to be an ordinary private
 * key: a credential that is extractable after all must not be spilled to disk
 * by the code path whose entire purpose is that it never is.
 */
#ifndef AZ_IOT_PAHO_KEY_CUSTODY_H
#define AZ_IOT_PAHO_KEY_CUSTODY_H

#include <stdbool.h>
#include <stddef.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* Per-connection custody state. Zero-initialize before the first prepare();
   * release() returns it to that state. */
  typedef struct az_iot_paho_key_custody
  {
    char* key_ref_path; /* temporary reference PEM to unlink, or NULL */
  } az_iot_paho_key_custody;

  /* True when `tls` asks for a key the adapter cannot simply read: a
   * non-extractable key reference, or a provider sign() hook. */
  bool az_iot_paho_key_custody_requested(const az_iot_mqtt_tls_options* tls);

  /* Resolve the custody request in `tls` into a private-key path Paho can load.
   *
   * On AZ_IOT_OK, *out_private_key_path is the path to pass to Paho: the
   * caller's own tls->client_key_path when no custody was requested, or a
   * reference PEM owned by `state` otherwise. Call release() either way.
   *
   * Failures:
   *   AZ_IOT_ERR_NOT_SUPPORTED - built without custody support, or the request
   *                              is one Paho cannot serve (a sign() hook with
   *                              no key URI), or crypto_engine_id names no
   *                              loadable OpenSSL provider.
   *   AZ_IOT_ERR_TLS           - the provider loaded but could not
   *                              resolve the URI, or resolved it to a key that
   *                              carries extractable private material.
   *   AZ_IOT_ERR_OUT_OF_MEMORY - could not allocate or create the reference
   *                              file. */
  az_iot_result az_iot_paho_key_custody_prepare(
      az_iot_paho_key_custody* state,
      const az_iot_mqtt_tls_options* tls,
      const char** out_private_key_path);

  /* Remove the reference file. The OpenSSL provider it was resolved through is
   * process-wide and stays loaded (see the .c). Safe on a zeroed or
   * already-released state. */
  void az_iot_paho_key_custody_release(az_iot_paho_key_custody* state);

#if defined(AZ_IOT_PAHO_KEY_CUSTODY)
  /* Does this buffer contain an extractable private key in PEM form, anywhere?
   *
   * The gate that stops prepare() writing real key material to disk. Exposed
   * here -- on an adapter-private header that is not installed -- only so the
   * unit tests can drive buffer shapes an OpenSSL encoder will not produce on
   * demand, such as a BEGIN line that is not at offset 0. `pem` need not be
   * NUL-terminated; `len` bounds it. */
  bool az_iot_paho_key_custody_pem_carries_private_key(const char* pem, size_t len);
#endif

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_PAHO_KEY_CUSTODY_H */
