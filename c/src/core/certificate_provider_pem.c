// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* ALLOCATION POLICY -- deliberate, documented exception to the SDK's
 * "no dynamic allocation in src/" rule.
 *
 * This is the *reference* filesystem PEM loader: an I/O boundary, in the same
 * category as the Paho/OpenSSL MQTT adapters (which are likewise permitted to
 * allocate). It reads variable-size cert/key/CA files into heap buffers ONCE at
 * init() -- never on any hot path -- so a device that has a filesystem can
 * bootstrap X.509 auth with minimal ceremony. Reading arbitrary-size files
 * without a hard size cap is what intrinsically requires the allocation.
 *
 * The core state machine (connection_client) and the feature clients remain
 * allocation-free. Constrained / no-filesystem targets should instead supply
 * their own az_iot_certificate_provider (TPM/HSM/secure element, or compiled-in
 * PEM) that performs no allocation and no file I/O -- the vtable contract is
 * identical. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_certificate_provider_pem.h"
#include "internal/log_internal.h"

/* Read entire file into a NUL-terminated heap string. Caller frees. */
static az_iot_result read_file_content(const char* path, char** out)
{
  *out = NULL;
  FILE* f = fopen(path, "rb");
  if (!f)
    return AZ_IOT_ERR_NOT_INITIALIZED;
  if (fseek(f, 0, SEEK_END) != 0)
  {
    fclose(f);
    return AZ_IOT_ERR_INTERNAL;
  }
  long len = ftell(f);
  if (len < 0)
  {
    fclose(f);
    return AZ_IOT_ERR_INTERNAL;
  }
  if (fseek(f, 0, SEEK_SET) != 0)
  {
    fclose(f);
    return AZ_IOT_ERR_INTERNAL;
  }

  char* buf = (char*)malloc((size_t)len + 1);
  if (!buf)
  {
    fclose(f);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  size_t n = fread(buf, 1, (size_t)len, f);
  fclose(f);
  if (n != (size_t)len)
  {
    free(buf);
    return AZ_IOT_ERR_INTERNAL;
  }
  buf[n] = '\0';
  *out = buf;
  return AZ_IOT_OK;
}

static az_iot_result pem_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  az_iot_certificate_provider_pem* m = (az_iot_certificate_provider_pem*)self;
  (void)role; /* static-cert provider: same material for bootstrap and operational */
  if (!m || !out)
    return AZ_IOT_ERR_INVALID_ARG;
  if (!m->loaded)
    return AZ_IOT_ERR_NOT_INITIALIZED;

  out->trusted_ca_pem = m->trusted_ca;
  out->client_cert_pem = m->client_cert;
  out->client_key_pem = m->client_key;
  out->client_key_password = m->key_password;
  out->trusted_ca_path = m->ca_path;
  out->client_cert_path = m->cert_path;
  out->client_key_path = m->key_path;
  out->client_key_uri = NULL;
  out->crypto_engine_id = NULL;
  return AZ_IOT_OK;
}

static void pem_release(az_iot_certificate_provider* self, az_iot_certificate_material* material)
{
  (void)self;
  (void)material;
}

static void pem_deinit_vtable(az_iot_certificate_provider* self)
{
  az_iot_certificate_provider_pem_destroy((az_iot_certificate_provider_pem*)self);
}

static const az_iot_certificate_provider_vtable s_pem_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = pem_load,
  .release = pem_release,
  .deinit = pem_deinit_vtable,
};

static char* dup_str(const char* s)
{
  if (!s)
    return NULL;
  size_t n = strlen(s);
  char* out = (char*)malloc(n + 1);
  if (!out)
    return NULL;
  memcpy(out, s, n + 1);
  return out;
}

void az_iot_certificate_provider_pem_destroy(az_iot_certificate_provider_pem* provider)
{
  if (!provider)
    return;
  free(provider->trusted_ca);
  free(provider->client_cert);
  free(provider->client_key);
  free(provider->key_password);
  free(provider->ca_path);
  free(provider->cert_path);
  free(provider->key_path);
  memset(provider, 0, sizeof(*provider));
}

az_iot_certificate_provider_pem_options az_iot_certificate_provider_pem_options_default(void)
{
  az_iot_certificate_provider_pem_options opts = { 0 };
  return opts;
}

az_iot_result az_iot_certificate_provider_pem_init(
    az_iot_certificate_provider_pem* provider,
    const az_iot_certificate_provider_pem_options* opts)
{
  if (!provider || !opts)
  {
    AZ_IOT_LOG_ERROR("certificate_provider_pem_init: invalid arguments");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!opts->client_cert_pem_path || !opts->client_cert_pem_path[0] || !opts->client_key_pem_path
      || !opts->client_key_pem_path[0])
  {
    AZ_IOT_LOG_ERROR(
        "certificate_provider_pem_init: client_cert_pem_path and client_key_pem_path are required");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(provider, 0, sizeof(*provider));
  provider->base.vtable = &s_pem_vtable;

  az_iot_result r = read_file_content(opts->client_cert_pem_path, &provider->client_cert);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("certificate_provider_pem_init: failed to read client cert file");
    az_iot_certificate_provider_pem_destroy(provider);
    return r;
  }

  r = read_file_content(opts->client_key_pem_path, &provider->client_key);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("certificate_provider_pem_init: failed to read client key file");
    az_iot_certificate_provider_pem_destroy(provider);
    return r;
  }

  if (opts->trusted_ca_pem_path && opts->trusted_ca_pem_path[0])
  {
    r = read_file_content(opts->trusted_ca_pem_path, &provider->trusted_ca);
    if (r != AZ_IOT_OK)
    {
      az_iot_certificate_provider_pem_destroy(provider);
      return r;
    }
  }

  if (opts->client_key_password)
  {
    provider->key_password = dup_str(opts->client_key_password);
    if (!provider->key_password)
    {
      az_iot_certificate_provider_pem_destroy(provider);
      return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
  }

  provider->cert_path = dup_str(opts->client_cert_pem_path);
  provider->key_path = dup_str(opts->client_key_pem_path);
  if (!provider->cert_path || !provider->key_path)
  {
    az_iot_certificate_provider_pem_destroy(provider);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  if (opts->trusted_ca_pem_path && opts->trusted_ca_pem_path[0])
  {
    provider->ca_path = dup_str(opts->trusted_ca_pem_path);
    if (!provider->ca_path)
    {
      az_iot_certificate_provider_pem_destroy(provider);
      return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
  }

  provider->loaded = true;
  return AZ_IOT_OK;
}
