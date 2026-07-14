// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include "az_iot_cert_embedded.h"

#include <stddef.h>

static az_iot_result embedded_load(
    az_iot_certificate_provider* self, az_iot_cert_role role, az_iot_certificate_material* out)
{
    az_iot_cert_embedded* p = (az_iot_cert_embedded*)self;
    (void)role; /* static-cert provider: same material for bootstrap and operational */
    out->trusted_ca_pem = p->ca_pem;
    out->client_cert_pem = p->cert_pem;
    out->client_key_pem = p->key_pem;
    out->client_key_password = NULL;
    /* No file paths: this is a memory-only provider. */
    out->trusted_ca_path = NULL;
    out->client_cert_path = NULL;
    out->client_key_path = NULL;
    /* No HSM key reference: memory-only provider. */
    out->client_key_uri = NULL;
    out->crypto_engine_id = NULL;
    return (p->cert_pem && p->key_pem) ? AZ_IOT_OK : AZ_IOT_ERR_INVALID_ARG;
}

static void embedded_release(
    az_iot_certificate_provider* self, az_iot_certificate_material* material)
{
    (void)self; (void)material; /* nothing to free; PEM is static firmware data */
}

static void embedded_destroy(az_iot_certificate_provider* self)
{
    (void)self;
}

static const az_iot_certificate_provider_vtable k_embedded_vtable = {
    .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
    .load = embedded_load,
    .release = embedded_release,
    .deinit = embedded_destroy,
};

void az_iot_cert_embedded_init(
    az_iot_cert_embedded* provider,
    const char* ca_pem, const char* cert_pem, const char* key_pem)
{
    provider->base.vtable = &k_embedded_vtable;
    provider->ca_pem = ca_pem;
    provider->cert_pem = cert_pem;
    provider->key_pem = key_pem;
}
