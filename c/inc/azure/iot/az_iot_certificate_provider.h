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
   * based enrollment. From vtable v3 a role can also authenticate with a SAS
   * token (see az_iot_credential_kind). See docs/eng/certificate-management.md. */

  /* Which identity load() should return (design decision D3). */
  typedef enum
  {
    AZ_IOT_CRED_BOOTSTRAP = 0, /* identity that authenticates to DPS         */
    AZ_IOT_CRED_OPERATIONAL /* identity that authenticates to the hub      */
  } az_iot_cert_role;

  /**
   * @brief How a role authenticates. Chosen per role by load().
   */
  typedef enum az_iot_credential_kind
  {
    /** @brief TLS client certificate. Default (zero). */
    AZ_IOT_CREDENTIAL_X509 = 0,
    /**
     * @brief SAS token in the MQTT password, signed through the vtable's
     * sign_sas(). The SDK builds the token and renews it before it expires.
     */
    AZ_IOT_CREDENTIAL_SAS
  } az_iot_credential_kind;

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
    /**
     * @brief Credential this role authenticates with.
     *
     * AZ_IOT_CREDENTIAL_SAS requires vtable version >= 3 and sign_sas(); the
     * client_cert_* / client_key_* / crypto_engine_id fields must then be NULL.
     * trusted_ca_* still apply: TLS always authenticates the server.
     * A violation fails the connect with AZ_IOT_ERR_CREDENTIAL_INCOMPLETE.
     */
    az_iot_credential_kind kind;
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
   * Each entry is the base64-encoded DER of one certificate exactly as received on
   * the wire (no PEM header/footer); a provider that persists the chain wraps each
   * entry in PEM. Passing base64 spans (rather than pre-wrapped PEM strings) lets
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
    const az_span* certificates; /* base64 DER certs, leaf first */
    size_t count;
  } az_iot_issued_certificate;

  typedef struct az_iot_certificate_provider az_iot_certificate_provider;

/* Vtable ABI version (D1). The client checks this before calling any hook added
 * after v1; a provider MUST set vtable->version to the value it was built with. */
#define AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION 3u

  typedef struct az_iot_certificate_provider_vtable
  {
    uint32_t version; /* = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION (D1) */

    /* v1 core. load() returns credential material for the requested role. */
    az_iot_result (*load)(
        az_iot_certificate_provider* self,
        az_iot_cert_role role,
        az_iot_certificate_material* out_material);
    void (*release)(az_iot_certificate_provider* self, az_iot_certificate_material* material);
    void (*deinit)(az_iot_certificate_provider* self);

    /* v2 CSR enrollment (optional; NULL get_csr => enrollment not supported). */
    az_iot_result (*get_csr)(
        az_iot_certificate_provider* self,
        const char* subject_common_name,
        az_iot_certificate_signing_request* out_csr);
    void (*release_csr)(az_iot_certificate_provider* self, az_iot_certificate_signing_request* csr);
    /* Persist the issued OPERATIONAL certificate chain. Deliberately typed on
     * az_iot_issued_certificate (a leaf-first cert CHAIN) rather than
     * az_iot_certificate_material: what is issued is a certificate chain only -
     * the matching private key was generated by the provider in get_csr() and
     * never transits this hook - so the single-cert material shape would not fit.
     * Each entry is base64 DER (the provider PEM-wraps to persist). The chain is
     * valid only for this call (see the type's lifetime note). */
    az_iot_result (*store_issued_certificate)(
        az_iot_certificate_provider* self,
        const az_iot_issued_certificate* issued);

    /* v2 non-extractable key custody (optional; D8). When present the TLS
     * adapter calls sign() instead of reading a private key: signs the
     * caller-provided digest, writing up to out_sig_cap bytes and setting
     * *out_sig_len.
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

    /**
     * @brief v3 SAS signing (optional; required when load() returns
     * AZ_IOT_CREDENTIAL_SAS for any role).
     *
     * Computes HMAC-SHA256 over @p data with the symmetric key of @p role. The
     * key never leaves the provider, so it can live in a TPM or HSM. The SDK
     * builds the string to sign (DPS registration or hub device resource) and
     * encodes the result into the token.
     *
     * @param[in] self          Provider.
     * @param[in] role          Role the token is for.
     * @param[in] data          String to sign.
     * @param[in] data_len      Length of @p data in bytes.
     * @param[out] out_hmac     Receives the raw (not base64) HMAC.
     * @param[in] out_hmac_cap  Capacity of @p out_hmac; at least 32.
     * @param[out] out_hmac_len Bytes written.
     * @return AZ_IOT_OK, or an error that fails the connect attempt.
     */
    az_iot_result (*sign_sas)(
        az_iot_certificate_provider* self,
        az_iot_cert_role role,
        const uint8_t* data,
        size_t data_len,
        uint8_t* out_hmac,
        size_t out_hmac_cap,
        size_t* out_hmac_len);
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
