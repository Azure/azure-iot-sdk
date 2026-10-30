// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "support/test_provider.h"

#include <string.h>

static az_iot_result test_provider_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    uint8_t index,
    az_iot_certificate_material* out_material)
{
  if (index != 0)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  (void)role;
  if (self == NULL || out_material == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(out_material, 0, sizeof(*out_material));
  out_material->trusted_ca_path = ((az_iot_test_provider*)self)->trusted_ca_path;
  return AZ_IOT_OK;
}

static void test_provider_release(
    az_iot_certificate_provider* self,
    az_iot_certificate_material* material)
{
  (void)self;
  (void)material;
}

static void test_provider_deinit(az_iot_certificate_provider* self) { (void)self; }

static const az_iot_certificate_provider_vtable k_test_provider_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = test_provider_load,
  .release = test_provider_release,
  .deinit = test_provider_deinit,
};

void az_iot_test_provider_init(az_iot_test_provider* provider, const char* trusted_ca_path)
{
  memset(provider, 0, sizeof(*provider));
  provider->base.vtable = &k_test_provider_vtable;
  provider->trusted_ca_path = trusted_ca_path;
}

az_iot_result az_iot_test_connection_client_init(
    az_iot_connection_client* client,
    const az_iot_connection_client_options* opts)
{
  /* Stateless, so one instance serves every client. */
  static az_iot_test_provider s_provider = { .base = { .vtable = &k_test_provider_vtable } };
  if (opts == NULL || opts->certificate_provider != NULL)
  {
    return az_iot_connection_client_init(client, opts);
  }
  az_iot_connection_client_options with_provider = *opts;
  with_provider.certificate_provider = &s_provider.base;
  return az_iot_connection_client_init(client, &with_provider);
}
