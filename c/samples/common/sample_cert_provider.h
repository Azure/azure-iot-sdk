// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* sample_cert_provider - a COMPLETE, app-owned az_iot_certificate_provider_t for
 * the samples, using platform-native crypto to issue the CSR:
 *   - Linux / non-Windows: OpenSSL 3.0+ (sample_csr_openssl.c)
 *   - Windows:             CNG / NCrypt   (sample_csr_cng.c)
 *
 * Unlike the shipped az_iot_certificate_provider_managed (which hides the crypto
 * inside the library), this lives in the samples tree so you can read and copy
 * the real CSR-issuance code for your own provider. It is intentionally
 * functionally similar to the managed provider.
 *
 * Key/cert handling: the operational private key is written as a PEM file at
 * operational_key_path and the issued chain to operational_cert_path, because
 * the bundled Paho MQTT adapter's TLS is OpenSSL-based on every platform and
 * loads both from files. (A production Windows integration could instead keep a
 * non-extractable CNG key and wire the provider's sign() hook.)
 */
#ifndef SAMPLE_CERT_PROVIDER_H
#define SAMPLE_CERT_PROVIDER_H

#include "azure/iot/az_iot_certificate_provider.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    const char* bootstrap_cert_path;    /* required: X.509 bootstrap cert (PEM) */
    const char* bootstrap_key_path;     /* required: bootstrap private key (PEM) */
    const char* trusted_ca_path;        /* may be NULL */
    const char* operational_key_path;   /* required: operational key (PEM; loaded or generated) */
    const char* operational_cert_path;  /* required: issued chain is persisted here */
} sample_cert_provider_options_t;

/* Caller-owned struct; fields are INTERNAL. */
typedef struct
{
    az_iot_certificate_provider_t base; /* MUST be first (vtable pointer) */
    char* bootstrap_cert_path;
    char* bootstrap_key_path;
    char* trusted_ca_path;
    char* operational_key_path;
    char* operational_cert_path;
    int   has_operational;
} sample_cert_provider_t;

/* Initialize the provider. Detects any operational cert already on disk (so a
 * prior enrollment survives restart). Returns AZ_IOT_ERR_INVALID_ARG on a
 * missing required path. Pass &provider.base wherever an
 * az_iot_certificate_provider_t* is expected. */
az_iot_result_t sample_cert_provider_init(
    sample_cert_provider_t* provider, const sample_cert_provider_options_t* opts);

/* Release heap-owned paths. Does NOT delete files on disk. */
void sample_cert_provider_deinit(sample_cert_provider_t* provider);

#ifdef __cplusplus
}
#endif

#endif /* SAMPLE_CERT_PROVIDER_H */
