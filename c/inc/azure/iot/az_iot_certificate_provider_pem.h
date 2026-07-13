// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Default file/PEM-loader implementation of az_iot_certificate_provider.
 *
 * Initialize via az_iot_certificate_provider_pem_init() with a set of file paths.
 * The first call to load() reads all configured files into heap buffers;
 * subsequent loads return the same buffered material (cheap). release() is
 * a no-op; the buffers live until destroy().
 *
 * This loader is the recommended starting point for X.509 device auth.
 * Production deployments that source cert material from a TPM/HSM/keyvault
 * should implement their own az_iot_certificate_provider with the same vtable
 * contract and pass it into the relevant client options.
 */
#ifndef AZ_IOT_CERTIFICATE_PROVIDER_PEM_H
#define AZ_IOT_CERTIFICATE_PROVIDER_PEM_H

#include <stdbool.h>

#include "az_iot_certificate_provider.h"
#include "az_iot_result.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct az_iot_certificate_provider_pem_options
{
    const char* trusted_ca_pem_path;     /* may be NULL                          */
    const char* client_cert_pem_path;    /* required                             */
    const char* client_key_pem_path;     /* required                             */
    const char* client_key_password;     /* may be NULL; copied verbatim         */
} az_iot_certificate_provider_pem_options;

/* Returns an options struct with all fields defaulted (every path NULL). Set at
 * least client_cert_pem_path and client_key_pem_path on the returned struct
 * before az_iot_certificate_provider_pem_init(). */
AZ_NODISCARD az_iot_certificate_provider_pem_options
az_iot_certificate_provider_pem_options_default(void);

/* Caller-owned PEM certificate provider struct. Fields are INTERNAL. */
typedef struct az_iot_certificate_provider_pem
{
    az_iot_certificate_provider base;    /* MUST be first (vtable pointer) */
    char* trusted_ca;
    char* client_cert;
    char* client_key;
    char* key_password;
    char* ca_path;
    char* cert_path;
    char* key_path;
    bool  loaded;
} az_iot_certificate_provider_pem;

/* Initialize a file-backed certificate provider. Reads cert/key files from
 * disk into heap buffers owned by the struct. Returns ERR_INVALID_ARG if
 * required paths are missing. The provider base pointer can be passed wherever
 * az_iot_certificate_provider* is expected. */
AZ_NODISCARD az_iot_result az_iot_certificate_provider_pem_init(
    az_iot_certificate_provider_pem* provider,
    const az_iot_certificate_provider_pem_options* opts);

/* Release heap-owned file buffers. Does NOT free the struct itself. */
void az_iot_certificate_provider_pem_destroy(
    az_iot_certificate_provider_pem* provider);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CERTIFICATE_PROVIDER_PEM_H */
