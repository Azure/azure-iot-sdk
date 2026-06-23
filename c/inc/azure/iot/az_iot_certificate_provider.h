// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_CERTIFICATE_PROVIDER_H
#define AZ_IOT_CERTIFICATE_PROVIDER_H

#include "az_iot_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pluggable certificate provider. The default implementation passes through
 * file/PEM material from configuration. Custom implementations can integrate TPM /
 * HSM / OS keystore. The hook is optional on Classic and (currently) optional on Next;
 * mandatory pluggable surface so platforms can wire mandatory rotation later. */
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
} az_iot_certificate_material_t;

typedef struct az_iot_certificate_provider_tag az_iot_certificate_provider_t;

typedef struct az_iot_certificate_provider_vtable_tag
{
    az_iot_result_t (*load)(az_iot_certificate_provider_t* self, az_iot_certificate_material_t* out_material);
    void         (*release)(az_iot_certificate_provider_t* self, az_iot_certificate_material_t* material);
    void         (*deinit)(az_iot_certificate_provider_t* self);
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
