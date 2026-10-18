// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* ALLOCATION POLICY -- deliberate, documented exception to the SDK's
 * "no dynamic allocation in src/" rule.
 *
 * This is the *reference* filesystem PEM loader: an I/O boundary, in the same
 * category as the Paho/OpenSSL MQTT adapters (which are likewise permitted to
 * allocate). It reads variable-size cert/CA files into heap buffers once at
 * init(), and the private key at load() -- connect attempts, never a hot path --
 * so a device that has a filesystem can bootstrap X.509 auth with minimal
 * ceremony. Reading arbitrary-size files without a hard size cap is what
 * intrinsically requires the allocation.
 *
 * The core state machine (connection_client) and the feature clients remain
 * allocation-free. Constrained / no-filesystem targets should instead supply
 * their own az_iot_certificate_provider (TPM/HSM/secure element, or compiled-in
 * PEM) that performs no allocation and no file I/O -- the vtable contract is
 * identical.
 *
 * az-iot-allow: malloc -- reference filesystem PEM loader; files have no size cap
 * az-iot-allow: free -- pairs with the malloc above */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_certificate_provider_pem.h"
#include "internal/crypto.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

/**
 * @brief Reads a whole file into a NUL-terminated heap buffer.
 *
 * @param[in]  path     File to read.
 * @param[out] out      The buffer; the caller frees it. NULL on failure.
 * @param[out] out_size Allocated size of @p *out (file length + 1); may be NULL.
 * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_INITIALIZED when the file cannot be opened;
 * AZ_IOT_ERR_OUT_OF_MEMORY; AZ_IOT_ERR_INTERNAL on a read error. A partial
 * read is wiped before it is freed.
 */
static az_iot_result read_file_content(const char* path, char** out, size_t* out_size)
{
  *out = NULL;
  if (out_size != NULL)
  {
    *out_size = 0;
  }
  FILE* f = fopen(path, "rb");
  if (!f)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }
  if (fseek(f, 0, SEEK_END) != 0)
  {
    (void)fclose(f);
    return AZ_IOT_ERR_INTERNAL;
  }
  long len = ftell(f);
  if (len < 0)
  {
    (void)fclose(f);
    return AZ_IOT_ERR_INTERNAL;
  }
  if (fseek(f, 0, SEEK_SET) != 0)
  {
    (void)fclose(f);
    return AZ_IOT_ERR_INTERNAL;
  }

  char* buf = (char*)malloc((size_t)len + 1);
  if (!buf)
  {
    (void)fclose(f);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  size_t n = fread(buf, 1, (size_t)len, f);
  (void)fclose(f);
  if (n != (size_t)len)
  {
    az_iot_crypto__wipe(buf, (size_t)len + 1);
    free(buf);
    return AZ_IOT_ERR_INTERNAL;
  }
  buf[n] = '\0';
  *out = buf;
  if (out_size != NULL)
  {
    *out_size = n + 1;
  }
  return AZ_IOT_OK;
}

/** @brief Wipes and frees the in-memory private key; drops every loan. */
static void drop_client_key(az_iot_certificate_provider_pem* m)
{
  if (m->client_key != NULL)
  {
    az_iot_crypto__wipe(m->client_key, m->client_key_size);
    free(m->client_key);
  }
  m->client_key = NULL;
  m->client_key_size = 0;
  m->client_key_refs = 0;
}

static az_iot_result pem_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    uint8_t index,
    az_iot_certificate_material* out)
{
  az_iot_certificate_provider_pem* m = (az_iot_certificate_provider_pem*)self;
  (void)role; /* static-cert provider: same material for bootstrap and operational */
  if (!m || !out)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (index != 0)
  {
    return AZ_IOT_ERR_NOT_FOUND; /* one certificate per role */
  }
  if (!m->loaded)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }
  if (m->client_key_refs == UINT32_MAX)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  /* The key is read per loan, so it is in memory only between load() and the
   * last release(). */
  if (m->client_key_refs == 0)
  {
    az_iot_result r = read_file_content(m->key_path, &m->client_key, &m->client_key_size);
    if (r != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CERT_PEM, "load: failed to read client key file");
      return r;
    }
  }
  m->client_key_refs++;

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

/* Ends a loan from pem_load(); the last one wipes and frees the key. The
 * material is cleared, so a second release() of it is a no-op. */
static void pem_release(az_iot_certificate_provider* self, az_iot_certificate_material* material)
{
  az_iot_certificate_provider_pem* m = (az_iot_certificate_provider_pem*)self;
  if (m == NULL || material == NULL)
  {
    return;
  }
  if (material->client_key_pem != NULL && material->client_key_pem == m->client_key
      && m->client_key_refs > 0)
  {
    m->client_key_refs--;
    if (m->client_key_refs == 0)
    {
      drop_client_key(m);
    }
  }
  memset(material, 0, sizeof(*material));
}

static void pem_deinit_vtable(az_iot_certificate_provider* self)
{
  az_iot_certificate_provider_pem_deinit((az_iot_certificate_provider_pem*)self);
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
  {
    return NULL;
  }
  size_t n = strlen(s);
  char* out = (char*)malloc(n + 1);
  if (!out)
  {
    return NULL;
  }
  memcpy(out, s, n + 1);
  return out;
}

void az_iot_certificate_provider_pem_deinit(az_iot_certificate_provider_pem* provider)
{
  if (!provider)
  {
    return;
  }
  drop_client_key(provider);
  if (provider->key_password != NULL)
  {
    az_iot_crypto__wipe(provider->key_password, strlen(provider->key_password));
  }
  free(provider->trusted_ca);
  free(provider->client_cert);
  free(provider->key_password);
  free(provider->ca_path);
  free(provider->cert_path);
  free(provider->key_path);
  memset(provider, 0, sizeof(*provider));
}

AZ_NODISCARD az_iot_certificate_provider_pem_options
az_iot_certificate_provider_pem_options_default(void)
{
  az_iot_certificate_provider_pem_options opts = { 0 };
  return opts;
}

AZ_NODISCARD az_iot_result az_iot_certificate_provider_pem_init(
    az_iot_certificate_provider_pem* provider,
    const az_iot_certificate_provider_pem_options* opts)
{
  if (!provider || !opts)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CERT_PEM, "init: invalid arguments");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!is_nonempty_cstr(opts->client_cert_pem_path) || !is_nonempty_cstr(opts->client_key_pem_path))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CERT_PEM,
        "init: client_cert_pem_path and client_key_pem_path are required");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(provider, 0, sizeof(*provider));
  provider->base.vtable = &s_pem_vtable;

  az_iot_result r = read_file_content(opts->client_cert_pem_path, &provider->client_cert, NULL);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CERT_PEM, "init: failed to read client cert file");
    az_iot_certificate_provider_pem_deinit(provider);
    return r;
  }

  /* Checks the key is readable; load() reads it again, so it is not kept. */
  r = read_file_content(
      opts->client_key_pem_path, &provider->client_key, &provider->client_key_size);
  drop_client_key(provider);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CERT_PEM, "init: failed to read client key file");
    az_iot_certificate_provider_pem_deinit(provider);
    return r;
  }

  if (is_nonempty_cstr(opts->trusted_ca_pem_path))
  {
    r = read_file_content(opts->trusted_ca_pem_path, &provider->trusted_ca, NULL);
    if (r != AZ_IOT_OK)
    {
      az_iot_certificate_provider_pem_deinit(provider);
      return r;
    }
  }

  if (opts->client_key_password)
  {
    provider->key_password = dup_str(opts->client_key_password);
    if (!provider->key_password)
    {
      az_iot_certificate_provider_pem_deinit(provider);
      return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
  }

  provider->cert_path = dup_str(opts->client_cert_pem_path);
  provider->key_path = dup_str(opts->client_key_pem_path);
  if (!provider->cert_path || !provider->key_path)
  {
    az_iot_certificate_provider_pem_deinit(provider);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  if (is_nonempty_cstr(opts->trusted_ca_pem_path))
  {
    provider->ca_path = dup_str(opts->trusted_ca_pem_path);
    if (!provider->ca_path)
    {
      az_iot_certificate_provider_pem_deinit(provider);
      return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
  }

  provider->loaded = true;
  return AZ_IOT_OK;
}
