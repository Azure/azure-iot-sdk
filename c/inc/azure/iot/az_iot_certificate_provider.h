// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_CERTIFICATE_PROVIDER_H
#define AZ_IOT_CERTIFICATE_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#include <azure/az_core.h>

#include "az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* Pluggable certificate provider. The default implementation passes through
   * file/PEM material from configuration. Custom implementations can integrate
   * TPM / HSM / OS keystore, and (optionally) certificate-signing-request (CSR)
   * based enrollment. See docs/eng/certificate-management.md.
   *
   * Key stability: while the connection client is open, the operational
   * private key does not change. get_csr() signs over that key, the one
   * load(AZ_IOT_CRED_OPERATIONAL) presents or will present once a chain for it
   * is stored, so resubmitting a request returns a chain that still fits. To
   * rotate the key, close the client, change the key (provider-specific; the
   * provider may be deinitialized and initialized again in place), then open
   * the client and enroll or renew. */

  /* Which identity load() should return (design decision D3). */
  typedef enum
  {
    AZ_IOT_CRED_BOOTSTRAP = 0, /* identity that authenticates to DPS         */
    AZ_IOT_CRED_OPERATIONAL /* DPS/Hub-issued operational cert, once held  */
  } az_iot_cert_role;

  typedef struct az_iot_certificate_material
  {
    const char* trusted_ca_pem; /* may be NULL */
    const char* client_cert_pem; /* required for X.509 auth */
    const char* client_key_pem; /* required for X.509 auth */
    const char* client_key_password; /* may be NULL */
    /* File paths - populated when the cert source is file-based. Adapters that
     * require file paths (e.g. Paho + OpenSSL) use these; adapters that can
     * load from memory use the PEM strings above. */
    const char* trusted_ca_path; /* may be NULL */
    const char* client_cert_path; /* may be NULL */
    const char* client_key_path; /* may be NULL */
    /* Non-extractable key backends (HSM / TPM / secure element; D8). When set,
     * client_key_pem/path are NULL and the TLS adapter signs the handshake
     * through this reference instead of reading a private key.
     *
     * Honoured today by the Paho adapter, which resolves client_key_uri
     * through the OpenSSL 3.x provider named by crypto_engine_id
     * (pkcs11-provider, tpm2-openssl). An adapter that cannot honour a
     * reference must fail the connect rather than proceed without a client
     * key; the rust_mqtt adapter has no TLS credential handling at all. */
    const char* client_key_uri; /* may be NULL; e.g. "pkcs11:token=...;object=..." */
    const char* crypto_engine_id; /* may be NULL; OpenSSL ENGINE/provider id: "pkcs11", "tpm2" */
    /** PSA Crypto key id of a non-extractable client key (mbedTLS), e.g. on ESP-IDF v6
     * the Digital Signature peripheral; 0: none. With it, client_key_pem/path/uri are
     * NULL. Honoured by the az_mqtt adapter with mbedTLS; other adapters fail the
     * connect with AZ_IOT_ERR_NOT_SUPPORTED. The handshake uses the key after
     * release(): it must stay usable until the connection client is deinitialized. */
    uint32_t client_key_psa_id;
  } az_iot_certificate_material;

  /* PKCS#10 certificate signing request produced by the provider. Base64-encoded
   * DER, no PEM headers/newlines - matches the DPS register "csr" field and the
   * Hub "$iothub/credentials" CSR "csr" field.
   *
   * Ownership: csr_base64 is owned by the provider that produced it via get_csr().
   * The connection client reads (copies) it synchronously while sending and then
   * calls release_csr(); the provider frees it there. Callers MUST NOT retain the
   * pointer past release_csr(). */
  typedef struct az_iot_certificate_signing_request
  {
    const char* csr_base64;
  } az_iot_certificate_signing_request;

  /* Operational certificate chain issued by the DPS- or Hub-linked CA (leaf first).
   * Each entry is one certificate exactly as received on the wire: base64 of its
   * DER, or base64 of a whole PEM certificate (the provisioning service sends
   * this form). A provider decodes each entry and, for DER, wraps it in PEM
   * before persisting. Passing base64 spans (rather than pre-wrapped PEM strings) lets
   * the connection client deliver the chain with ZERO heap allocation - the spans
   * point directly into the client's receive buffer.
   *
   * Ownership/lifetime: the array and the spans it holds point into the connection
   * client's receive buffer and are valid ONLY for the duration of the
   * store_issued_certificate() call / on_operational_certificate callback in which
   * they are delivered. A provider or app that needs to retain the chain (e.g. to
   * persist it) MUST copy the bytes; storing the spans yields dangling reads. */
  typedef struct az_iot_issued_certificate
  {
    const az_span* certificates; /* base64 DER or PEM certs, leaf first */
    size_t count;
  } az_iot_issued_certificate;

  typedef struct az_iot_certificate_provider az_iot_certificate_provider;

/* Vtable ABI version (D1). The client checks this before calling any hook added
 * after v1; a provider MUST set vtable->version to the value it was built with. */
#define AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION 2u

  typedef struct az_iot_certificate_provider_vtable
  {
    uint32_t version; /* = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION (D1) */

    /* v1 core. load() returns the @p index-th certificate for @p role,
     * index 0 first; the client tries them in order when the service rejects
     * one, and remembers the index that connected.
     * AZ_IOT_ERR_NOT_FOUND: no certificate at @p index (past the last one, or
     * none for the role); for index 0, trusted_ca_pem/_path may still be set
     * (valid while the provider lives), and are then used for a role that
     * authenticates with SAS. Any other error fails the attempt, except while
     * the client checks an index > 0 during fallback: there it ends the
     * certificates, like AZ_IOT_ERR_NOT_FOUND, and is logged. The client
     * asks for at most AZ_IOT_MAX_CERTS_PER_ROLE indexes. A provider with one
     * certificate per role returns AZ_IOT_ERR_NOT_FOUND for index > 0. */
    az_iot_result (*load)(
        az_iot_certificate_provider* self,
        az_iot_cert_role role,
        uint8_t index,
        az_iot_certificate_material* out_material);
    /** Ends the use of @p material from a successful load(); called once per
     * such load(), and not after a failed one. Material for a connection
     * attempt is released once the adapter's connect() has copied it, or when
     * the attempt ends before connect(); material loaded only to validate or
     * probe certificates is released at once. @p material itself is caller
     * storage; the provider may then free what it references and SHOULD wipe
     * any in-memory private key or password it handed out (as the PEM provider
     * does) unless it still needs them;
     * deinit() wipes any it kept. A non-extractable key (client_key_uri or
     * sign()) keeps the key out of process memory altogether. */
    void (*release)(az_iot_certificate_provider* self, az_iot_certificate_material* material);
    void (*deinit)(az_iot_certificate_provider* self);

    /* v2 CSR enrollment (optional; NULL get_csr => enrollment not supported).
     * get_csr() signs over the provider's operational key (see key stability
     * above); repeated calls use the same key. */
    az_iot_result (*get_csr)(
        az_iot_certificate_provider* self,
        const char* subject_common_name,
        az_iot_certificate_signing_request* out_csr);
    void (*release_csr)(az_iot_certificate_provider* self, az_iot_certificate_signing_request* csr);
    /* Persist the issued OPERATIONAL certificate chain. Deliberately typed on
     * az_iot_issued_certificate (a leaf-first cert CHAIN) rather than
     * az_iot_certificate_material: what is issued is a certificate chain only -
     * the matching private key is the provider's operational key and never
     * transits this hook - so the single-cert material shape would not fit.
     * Each entry is base64 DER or PEM (see az_iot_issued_certificate). The chain is
     * valid only for this call (see the type's lifetime note).
     *
     * A chain whose leaf does not certify the operational key is refused
     * (AZ_IOT_ERR_INVALID_ARG). Must be all-or-nothing: on failure,
     * load(AZ_IOT_CRED_OPERATIONAL) keeps returning the previous credential, so
     * a failed renewal does not break the next connect. */
    az_iot_result (*store_issued_certificate)(
        az_iot_certificate_provider* self,
        const az_iot_issued_certificate* issued);

    /* v2 non-extractable key custody (optional; D8). When present the TLS
     * adapter calls sign() instead of reading a private key: signs the
     * caller-provided digest, writing up to out_sig_cap bytes and setting
     * *out_sig_len. Called only from inside connection client calls, on the thread
     * making them (az_iot_mqtt_sign_callback).
     *
     * The connection client forwards this to the adapter (through
     * az_iot_mqtt_tls_options::sign) when vtable->version >= 2 and the slot is
     * set. It is the route for stacks with no engine/provider abstraction;
     * Paho is not one of them -- it exposes no TLS key callback and refuses a
     * sign()-only credential -- so this needs a BYO adapter. Use
     * client_key_uri + crypto_engine_id with Paho. */
    az_iot_result (*sign)(
        az_iot_certificate_provider* self,
        const uint8_t* digest,
        size_t digest_len,
        uint8_t* out_sig,
        size_t out_sig_cap,
        size_t* out_sig_len);
  } az_iot_certificate_provider_vtable;

  struct az_iot_certificate_provider
  {
    const az_iot_certificate_provider_vtable* vtable;
    /* implementation state follows */
  };

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CERTIFICATE_PROVIDER_H */
