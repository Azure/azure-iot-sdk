// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_CERTIFICATE_PROVIDER_H
#define AZ_IOT_CERTIFICATE_PROVIDER_H

#include <stddef.h>
#include <stdint.h>

#include "az_iot_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pluggable certificate provider. The default implementation passes through
 * file/PEM material from configuration. Custom implementations can integrate
 * TPM / HSM / OS keystore, and (optionally) certificate-signing-request (CSR)
 * based enrollment. See docs/eng/certificate-management.md. */

/* Which identity load() should return (design decision D3). */
typedef enum
{
    AZ_IOT_CRED_BOOTSTRAP = 0,   /* identity that authenticates to DPS         */
    AZ_IOT_CRED_OPERATIONAL      /* DPS/Hub-issued operational cert, once held  */
} az_iot_cert_role_t;

typedef struct az_iot_certificate_material_tag
{
    const char* trusted_ca_pem;        /* may be NULL */
    const char* client_cert_pem;       /* required for X.509 auth */
    const char* client_key_pem;        /* required for X.509 auth */
    const char* client_key_password;   /* may be NULL */
    /* File paths - populated when the cert source is file-based. Adapters that
     * require file paths (e.g. Paho + OpenSSL) use these; adapters that can
     * load from memory use the PEM strings above. */
    const char* trusted_ca_path;       /* may be NULL */
    const char* client_cert_path;      /* may be NULL */
    const char* client_key_path;       /* may be NULL */
    /* Non-extractable key backends (HSM / TPM / secure element; D8). When set,
     * client_key_pem/path are NULL and the TLS adapter uses this reference to
     * sign during the handshake instead of reading a private key. */
    const char* client_key_uri;        /* may be NULL; e.g. "pkcs11:token=...;object=..." */
    const char* crypto_engine_id;      /* may be NULL; OpenSSL ENGINE/provider id: "pkcs11", "tpm2" */
} az_iot_certificate_material_t;

/* PKCS#10 certificate signing request produced by the provider. Base64-encoded
 * DER, no PEM headers/newlines - matches the DPS register "csr" field and the
 * Hub "$iothub/credentials" CSR "csr" field. */
typedef struct az_iot_certificate_signing_request_tag
{
    const char* csr_base64;
} az_iot_certificate_signing_request_t;

/* Operational certificate chain issued by the DPS- or Hub-linked CA (leaf first). */
typedef struct az_iot_issued_certificate_tag
{
    const char* const* client_cert_chain_pem;   /* array of PEM certs, leaf first */
    size_t             count;
} az_iot_issued_certificate_t;

typedef struct az_iot_certificate_provider_tag az_iot_certificate_provider_t;

/* Vtable ABI version (D1). The client checks this before calling any hook added
 * after v1; a provider MUST set vtable->version to the value it was built with. */
#define AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION 2u

typedef struct az_iot_certificate_provider_vtable_tag
{
    uint32_t version;   /* = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION (D1) */

    /* v1 core. load() returns credential material for the requested role. */
    az_iot_result_t (*load)(az_iot_certificate_provider_t* self,
                            az_iot_cert_role_t role,
                            az_iot_certificate_material_t* out_material);
    void            (*release)(az_iot_certificate_provider_t* self, az_iot_certificate_material_t* material);
    void            (*deinit)(az_iot_certificate_provider_t* self);

    /* v2 CSR enrollment (optional; NULL get_csr => enrollment not supported). */
    az_iot_result_t (*get_csr)(az_iot_certificate_provider_t* self,
                               const char* subject_common_name,
                               az_iot_certificate_signing_request_t* out_csr);
    void            (*release_csr)(az_iot_certificate_provider_t* self,
                                   az_iot_certificate_signing_request_t* csr);
    az_iot_result_t (*store_issued_certificate)(az_iot_certificate_provider_t* self,
                                                const az_iot_issued_certificate_t* issued);

    /* v2 non-extractable key custody (optional; D8). When present the TLS adapter
     * calls sign() instead of reading a private key: signs the caller-provided
     * digest, writing up to out_sig_cap bytes and setting *out_sig_len. */
    az_iot_result_t (*sign)(az_iot_certificate_provider_t* self,
                            const uint8_t* digest, size_t digest_len,
                            uint8_t* out_sig, size_t out_sig_cap, size_t* out_sig_len);
} az_iot_certificate_provider_vtable_t;

struct az_iot_certificate_provider_tag
{
    const az_iot_certificate_provider_vtable_t* vtable;
    /* implementation state follows */
};

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CERTIFICATE_PROVIDER_H */
