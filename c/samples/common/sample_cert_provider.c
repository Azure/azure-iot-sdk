// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* sample_cert_provider - vtable glue and cert persistence. The platform-native
 * CSR issuance lives in sample_csr_openssl.c / sample_csr_cng.c. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS /* this sample uses fopen for cert persistence */
#endif

#include "sample_cert_provider.h"
#include "sample_csr_backend.h"
#include "sample_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PEM framing written around each base64 DER certificate the service issues. */
#define PEM_CERT_BEGIN "-----BEGIN CERTIFICATE-----\n"
#define PEM_CERT_END "\n-----END CERTIFICATE-----\n"

static char* dup_str(const char* s)
{
  if (!s)
  {
    return NULL;
  }
  size_t n = strlen(s) + 1;
  char* out = (char*)malloc(n);
  if (out)
  {
    memcpy(out, s, n);
  }
  return out;
}

/* True only when the file holds a non-empty PEM certificate. A zero-length or
 * partial file (e.g. a crash mid-write) must not be taken for a usable identity. */
static int cert_file_has_pem(const char* path)
{
  FILE* f = fopen(path, "rb");
  if (!f)
  {
    return 0;
  }
  char buf[64] = { 0 };
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  (void)fclose(f);
  return (n > 0) && (strstr(buf, "-----BEGIN CERTIFICATE-----") != NULL);
}

static az_iot_result provider_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  sample_cert_provider* p = (sample_cert_provider*)self;
  if (!p || !out)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(out, 0, sizeof(*out));
  out->trusted_ca_path = p->trusted_ca_path;

  if (role == AZ_IOT_CRED_OPERATIONAL)
  {
    if (!p->has_operational)
    {
      return AZ_IOT_ERR_NOT_FOUND;
    }
    out->client_cert_path = p->operational_cert_path;
    out->client_key_path = p->operational_key_path;
  }
  else
  {
    out->client_cert_path = p->bootstrap_cert_path;
    out->client_key_path = p->bootstrap_key_path;
  }
  return AZ_IOT_OK;
}

static void provider_release(
    az_iot_certificate_provider* self,
    az_iot_certificate_material* material)
{
  (void)self;
  (void)material; /* paths are owned by the provider struct */
}

static az_iot_result provider_get_csr(
    az_iot_certificate_provider* self,
    const char* subject_common_name,
    az_iot_certificate_signing_request* out_csr)
{
  sample_cert_provider* p = (sample_cert_provider*)self;
  if (!p || !out_csr)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  char* csr = NULL;
  if (sample_csr_backend_get_csr(p->operational_key_path, subject_common_name, &csr) != 0 || !csr)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  out_csr->csr_base64 = csr; /* handed to release_csr to free */
  return AZ_IOT_OK;
}

static void provider_release_csr(
    az_iot_certificate_provider* self,
    az_iot_certificate_signing_request* csr)
{
  (void)self;
  if (csr && csr->csr_base64)
  {
    free((void*)csr->csr_base64);
    csr->csr_base64 = NULL;
  }
}

static az_iot_result provider_store(
    az_iot_certificate_provider* self,
    const az_iot_issued_certificate* issued)
{
  sample_cert_provider* p = (sample_cert_provider*)self;
  if (!p || !issued || !issued->certificates || issued->count == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  FILE* f = sample_fopen_private(p->operational_cert_path);
  if (!f)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  /* PEM-wrap each base64 DER cert (leaf first) into the operational cert file. */
  az_iot_result rc = AZ_IOT_OK;
  for (size_t i = 0; i < issued->count; ++i)
  {
    az_span cert = issued->certificates[i];
    size_t len = (size_t)az_span_size(cert);
    if (len == 0)
    {
      continue;
    }
    if (fputs(PEM_CERT_BEGIN, f) < 0 || fwrite(az_span_ptr(cert), 1, len, f) != len
        || fputs(PEM_CERT_END, f) < 0)
    {
      rc = AZ_IOT_ERR_INTERNAL;
      break;
    }
  }
  if (fclose(f) != 0)
  {
    rc = AZ_IOT_ERR_INTERNAL;
  }

  if (rc == AZ_IOT_OK)
  {
    p->has_operational = 1;
  }
  return rc;
}

static void provider_deinit_vtable(az_iot_certificate_provider* self)
{
  sample_cert_provider_deinit((sample_cert_provider*)self);
}

static const az_iot_certificate_provider_vtable s_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = provider_load,
  .release = provider_release,
  .deinit = provider_deinit_vtable,
  .get_csr = provider_get_csr,
  .release_csr = provider_release_csr,
  .store_issued_certificate = provider_store,
};

void sample_cert_provider_deinit(sample_cert_provider* provider)
{
  if (!provider)
  {
    return;
  }
  free(provider->bootstrap_cert_path);
  free(provider->bootstrap_key_path);
  free(provider->trusted_ca_path);
  free(provider->operational_key_path);
  free(provider->operational_cert_path);
  memset(provider, 0, sizeof(*provider));
}

az_iot_result sample_cert_provider_init(
    sample_cert_provider* provider,
    const sample_cert_provider_options* opts)
{
  if (!provider || !opts)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!opts->bootstrap_cert_path || !opts->bootstrap_cert_path[0] || !opts->bootstrap_key_path
      || !opts->bootstrap_key_path[0] || !opts->operational_key_path
      || !opts->operational_key_path[0] || !opts->operational_cert_path
      || !opts->operational_cert_path[0])
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(provider, 0, sizeof(*provider));
  provider->base.vtable = &s_vtable;
  provider->bootstrap_cert_path = dup_str(opts->bootstrap_cert_path);
  provider->bootstrap_key_path = dup_str(opts->bootstrap_key_path);
  provider->operational_key_path = dup_str(opts->operational_key_path);
  provider->operational_cert_path = dup_str(opts->operational_cert_path);
  if (!provider->bootstrap_cert_path || !provider->bootstrap_key_path
      || !provider->operational_key_path || !provider->operational_cert_path)
  {
    sample_cert_provider_deinit(provider);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  if (opts->trusted_ca_path && opts->trusted_ca_path[0])
  {
    provider->trusted_ca_path = dup_str(opts->trusted_ca_path);
    if (!provider->trusted_ca_path)
    {
      sample_cert_provider_deinit(provider);
      return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
  }

  provider->has_operational = cert_file_has_pem(provider->operational_cert_path);
  return AZ_IOT_OK;
}
