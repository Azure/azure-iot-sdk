// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* sample_cert_provider - vtable glue and cert persistence. CSR issuance lives
 * in sample_csr_openssl.c. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS /* this sample uses fopen for cert persistence */
#endif
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L /* mkstemp, fdopen, fchmod */
#endif

#include "sample_cert_provider.h"
#include "sample_csr_backend.h"
#include "sample_utils.h"

#include <azure/core/az_base64.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* PEM framing written around each base64 DER certificate the service issues. */
#define PEM_CERT_BEGIN "-----BEGIN CERTIFICATE-----\n"
#define PEM_CERT_END "\n-----END CERTIFICATE-----\n"

/* Room for the unique suffix appended to the destination path for the
 * temporary file: ".XXXXXX" (POSIX) or ".<8 hex>.tmp" (Windows), plus NUL. */
#define SAMPLE_TMP_SUFFIX_MAX 16

/* Create a new, uniquely named file next to @p path for writing, failing
 * rather than opening anything already there (a file or a link). Returns the
 * stream and its path in *out_tmp (heap), or NULL. */
static FILE* sample_create_unique(const char* path, char** out_tmp)
{
  size_t cap = strlen(path) + SAMPLE_TMP_SUFFIX_MAX;
  char* tmp = (char*)malloc(cap);
  FILE* f = NULL;
  *out_tmp = NULL;
  if (!tmp)
  {
    return NULL;
  }
#ifdef _WIN32
  unsigned long seed
      = (unsigned long)GetTickCount64() ^ ((unsigned long)GetCurrentProcessId() << 16);
  for (int i = 0; i < 16 && !f; ++i)
  {
    seed = seed * 1103515245UL + 12345UL;
    (void)snprintf(tmp, cap, "%s.%08lx.tmp", path, seed & 0xFFFFFFFFUL);
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE || h == NULL)
    {
      if (GetLastError() != ERROR_FILE_EXISTS)
      {
        break;
      }
      continue;
    }
    int fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY);
    if (fd < 0)
    {
      CloseHandle(h);
      (void)DeleteFileA(tmp);
      break;
    }
    f = _fdopen(fd, "wb");
    if (!f)
    {
      _close(fd);
      (void)DeleteFileA(tmp);
      break;
    }
  }
#else
  (void)snprintf(tmp, cap, "%s.XXXXXX", path);
  int fd = mkstemp(tmp); /* O_CREAT|O_EXCL */
  if (fd >= 0)
  {
    if (fchmod(fd, S_IRUSR | S_IWUSR) == 0)
    {
      f = fdopen(fd, "wb");
    }
    if (!f)
    {
      (void)close(fd);
      (void)unlink(tmp);
    }
  }
#endif
  if (!f)
  {
    free(tmp);
    return NULL;
  }
  *out_tmp = tmp;
  return f;
}

/* Replace @p to with @p from in one step. Returns nonzero on success. */
static int sample_replace_file(const char* from, const char* to)
{
#ifdef _WIN32
  return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return rename(from, to) == 0;
#endif
}

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
static az_iot_result provider_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    uint8_t index,
    az_iot_certificate_material* out)
{
  sample_cert_provider* p = (sample_cert_provider*)self;
  if (!p || !out)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (index != 0)
  {
    return AZ_IOT_ERR_NOT_FOUND; /* one certificate per role */
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

/* Write one issued entry to @p f as PEM. The service sends base64 of either a
 * PEM certificate (written decoded) or of DER (wrapped in PEM lines). */
static az_iot_result write_cert_pem(FILE* f, az_span cert)
{
  static const char pem_b64[] = "LS0tLS1CRUdJTi"; /* base64 of "-----BEGIN" */
  int32_t n = az_span_size(cert);
  if (n < (int32_t)(sizeof(pem_b64) - 1)
      || memcmp(az_span_ptr(cert), pem_b64, sizeof(pem_b64) - 1) != 0)
  {
    return (fputs(PEM_CERT_BEGIN, f) < 0 || fwrite(az_span_ptr(cert), 1, (size_t)n, f) != (size_t)n
            || fputs(PEM_CERT_END, f) < 0)
        ? AZ_IOT_ERR_INTERNAL
        : AZ_IOT_OK;
  }
  int32_t cap = (n / 4) * 3 + 3;
  uint8_t* pem = (uint8_t*)malloc((size_t)cap);
  if (!pem)
  {
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  int32_t written = 0;
  az_iot_result rc = AZ_IOT_ERR_INVALID_ARG;
  if (az_result_succeeded(az_base64_decode(az_span_create(pem, cap), cert, &written)) && written > 0
      && written <= cap)
  {
    rc = (fwrite(pem, 1, (size_t)written, f) == (size_t)written
          && (pem[written - 1] == '\n' || fputc('\n', f) != EOF))
        ? AZ_IOT_OK
        : AZ_IOT_ERR_INTERNAL;
  }
  free(pem);
  return rc;
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
  /* Every entry must be one certificate, and the leaf must certify the
   * operational key, or the next connect fails. */
  if (!sample_csr_backend_cert_matches_key(
          p->operational_key_path,
          (const char*)az_span_ptr(issued->certificates[0]),
          (size_t)az_span_size(issued->certificates[0])))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  for (size_t i = 1; i < issued->count; ++i)
  {
    if (!sample_csr_backend_is_cert(
            (const char*)az_span_ptr(issued->certificates[i]),
            (size_t)az_span_size(issued->certificates[i])))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }

  /* All-or-nothing: write a new, uniquely named file next to the destination,
   * then replace the destination with it, so a failure keeps the previous
   * chain. A production provider should also restrict its ACL on Windows
   * (see the managed provider). */
  char* tmp = NULL;
  FILE* f = sample_create_unique(p->operational_cert_path, &tmp);
  if (!f)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  /* Write each cert (leaf first) as PEM into the temporary file. */
  az_iot_result rc = AZ_IOT_OK;
  for (size_t i = 0; i < issued->count; ++i)
  {
    az_span cert = issued->certificates[i];
    if (az_span_size(cert) <= 0)
    {
      rc = AZ_IOT_ERR_INVALID_ARG; /* every issued entry must be a certificate */
      break;
    }
    rc = write_cert_pem(f, cert);
    if (rc != AZ_IOT_OK)
    {
      break;
    }
  }
  if (fflush(f) != 0)
  {
    rc = AZ_IOT_ERR_INTERNAL;
  }
  if (fclose(f) != 0)
  {
    rc = AZ_IOT_ERR_INTERNAL;
  }
  if (rc == AZ_IOT_OK && !sample_replace_file(tmp, p->operational_cert_path))
  {
    rc = AZ_IOT_ERR_INTERNAL;
  }
  if (rc != AZ_IOT_OK)
  {
    (void)remove(tmp);
  }
  free(tmp);

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

  /* A stored chain is used only while it still certifies the key. */
  provider->has_operational = sample_csr_backend_cert_file_matches_key(
      provider->operational_key_path, provider->operational_cert_path);
  return AZ_IOT_OK;
}
