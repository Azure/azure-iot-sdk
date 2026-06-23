// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include "az_iot_cert_embedded.h"

#include <stddef.h>

static az_iot_result_t embedded_load(
    az_iot_certificate_provider_t* self, az_iot_certificate_material_t* out)
{
    az_iot_cert_embedded_t* p = (az_iot_cert_embedded_t*)self;
    out->trusted_ca_pem = p->ca_pem;
    out->client_cert_pem = p->cert_pem;
    out->client_key_pem = p->key_pem;
    out->client_key_password = NULL;
    /* No file paths: this is a memory-only provider. */
    out->trusted_ca_path = NULL;
    out->client_cert_path = NULL;
    out->client_key_path = NULL;
    return (p->cert_pem && p->key_pem) ? AZ_IOT_OK : AZ_IOT_ERR_INVALID_ARG;
}

static void embedded_release(
    az_iot_certificate_provider_t* self, az_iot_certificate_material_t* material)
{
    (void)self; (void)material; /* nothing to free; PEM is static firmware data */
}

static void embedded_deinit(az_iot_certificate_provider_t* self)
{
    (void)self;
}

static const az_iot_certificate_provider_vtable_t k_embedded_vtable = {
    .load = embedded_load,
    .release = embedded_release,
    .deinit = embedded_deinit,
};

void az_iot_cert_embedded_init(
    az_iot_cert_embedded_t* provider,
    const char* ca_pem, const char* cert_pem, const char* key_pem)
{
    provider->base.vtable = &k_embedded_vtable;
    provider->ca_pem = ca_pem;
    provider->cert_pem = cert_pem;
    provider->key_pem = key_pem;
}
