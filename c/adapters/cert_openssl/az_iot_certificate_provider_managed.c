// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* OpenSSL 3.0+ implementation of the "managed" certificate provider (D5).
 *
 * Uses only public OpenSSL 3.0 APIs: EVP_PKEY_Q_keygen for key generation,
 * the X509_REQ_* family for PKCS#10, and BIO for file I/O (so this file needs
 * no CRT fopen and stays clean under MSVC /W4 /WX). */
#include "az_iot_certificate_provider_managed.h"

#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

/* Operational key parameters. */
#define MANAGED_RSA_KEY_BITS 2048
#define MANAGED_EC_CURVE_NAME "P-256"

/* An optional C string a caller may leave unset either way: NULL and "" both
 * mean "not supplied". Stated positively so call sites read as "this was
 * supplied" rather than a negated absence; the parameter is parenthesised so an
 * expression argument cannot misparse.
 *
 * Duplicated from c/src/core/internal/span_writer.h on purpose: adapters may
 * include only public azure/iot headers, so they cannot reach that one. The
 * Paho adapter carries the same copy for the same reason. */
#define is_nonempty_cstr(s) ((s) != NULL && (s)[0] != '\0')

/* What a PEM certificate starts with, used both to frame one and to tell an
 * already-PEM payload from base64 DER after decoding. */
#define PEM_CERT_PREFIX "-----BEGIN CERTIFICATE-----"

/* PEM framing written around a certificate the service issues as base64 DER. */
#define PEM_CERT_BEGIN PEM_CERT_PREFIX "\n"
#define PEM_CERT_END "\n-----END CERTIFICATE-----\n"

/* Longest base64 line a PEM body may contain (RFC 7468 recommends 64). */
#define PEM_LINE_LEN 64

/* Encoded length (excluding NUL) of base64 over `binary_len` bytes. */
#define BASE64_ENCODED_LEN(binary_len) ((((binary_len) + 2) / 3) * 4)

/* --------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

static char* dup_str(const char* s)
{
  if (!s)
  {
    return NULL;
  }
  size_t n = strlen(s);
  char* out = malloc(n + 1);
  if (!out)
  {
    return NULL;
  }
  memcpy(out, s, n + 1);
  return out;
}

static EVP_PKEY* generate_key(int key_type)
{
  if (key_type == AZ_IOT_MANAGED_KEY_RSA_2048)
  {
    return EVP_PKEY_Q_keygen(NULL, NULL, "RSA", (size_t)MANAGED_RSA_KEY_BITS);
  }
  return EVP_PKEY_Q_keygen(NULL, NULL, "EC", MANAGED_EC_CURVE_NAME);
}

static EVP_PKEY* load_key_file(const char* path)
{
  BIO* b = BIO_new_file(path, "rb");
  if (!b)
  {
    ERR_clear_error();
    return NULL;
  }
  EVP_PKEY* k = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
  BIO_free(b);
  return k;
}

static az_iot_result write_key_file(const char* path, EVP_PKEY* key)
{
  BIO* b = BIO_new_file(path, "wb");
  if (!b)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  int ok = PEM_write_bio_PrivateKey(b, key, NULL, NULL, 0, NULL, NULL);
  BIO_free(b);
  return (ok == 1) ? AZ_IOT_OK : AZ_IOT_ERR_INTERNAL;
}

static bool operational_cert_is_valid(const char* path)
{
  /* Treat the operational cert as present only when the file holds at least one
   * parseable PEM certificate. A zero-length or partially-written file (e.g. a
   * crash mid-write, or a pre-created empty file) must NOT be mistaken for a
   * usable identity, or TLS would later fail to load a valid chain. */
  BIO* b = BIO_new_file(path, "rb");
  if (!b)
  {
    ERR_clear_error();
    return false;
  }
  X509* cert = PEM_read_bio_X509(b, NULL, NULL, NULL);
  BIO_free(b);
  if (!cert)
  {
    ERR_clear_error();
    return false;
  }
  X509_free(cert);
  return true;
}

/* --------------------------------------------------------------------------
 * vtable hooks
 * ------------------------------------------------------------------------ */

static az_iot_result managed_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  az_iot_certificate_provider_managed* m = (az_iot_certificate_provider_managed*)self;
  if (!m || !out)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!m->loaded)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  memset(out, 0, sizeof(*out));
  out->trusted_ca_path = m->trusted_ca_path;

  if (role == AZ_IOT_CRED_OPERATIONAL)
  {
    if (!m->has_operational)
    {
      return AZ_IOT_ERR_NOT_FOUND;
    }
    out->client_cert_path = m->operational_cert_path;
    out->client_key_path = m->operational_key_path;
  }
  else
  {
    out->client_cert_path = m->bootstrap_cert_path;
    out->client_key_path = m->bootstrap_key_path;
  }
  return AZ_IOT_OK;
}

static void managed_release(
    az_iot_certificate_provider* self,
    az_iot_certificate_material* material)
{
  (void)self;
  (void)material; /* paths are owned by the provider struct */
}

static az_iot_result managed_get_csr(
    az_iot_certificate_provider* self,
    const char* subject_common_name,
    az_iot_certificate_signing_request* out_csr)
{
  az_iot_certificate_provider_managed* m = (az_iot_certificate_provider_managed*)self;
  if (!m || !out_csr)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!m->loaded || !m->operational_key)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  out_csr->csr_base64 = NULL;

  EVP_PKEY* key = (EVP_PKEY*)m->operational_key;
  X509_REQ* req = NULL;
  X509_NAME* name = NULL;
  unsigned char* der = NULL;
  char* b64 = NULL;
  az_iot_result rc = AZ_IOT_ERR_INTERNAL;

  req = X509_REQ_new();
  if (!req)
  {
    goto done;
  }
  if (X509_REQ_set_version(req, 0L) != 1)
  {
    goto done; /* PKCS#10 v1 */
  }

  name = X509_NAME_new();
  if (!name)
  {
    goto done;
  }
  {
    const char* cn
        = is_nonempty_cstr(subject_common_name) ? subject_common_name : "azure-iot-device";
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, (const unsigned char*)cn, -1, -1, 0)
        != 1)
    {
      goto done;
    }
  }
  if (X509_REQ_set_subject_name(req, name) != 1)
  {
    goto done;
  }
  if (X509_REQ_set_pubkey(req, key) != 1)
  {
    goto done;
  }
  if (X509_REQ_sign(req, key, EVP_sha256()) == 0)
  {
    goto done;
  }

  {
    int der_len = i2d_X509_REQ(req, &der);
    if (der_len <= 0 || der == NULL)
    {
      goto done;
    }

    size_t b64_cap = BASE64_ENCODED_LEN((size_t)der_len) + 1;
    b64 = malloc(b64_cap);
    if (!b64)
    {
      rc = AZ_IOT_ERR_OUT_OF_MEMORY;
      goto done;
    }
    int b64_len = EVP_EncodeBlock((unsigned char*)b64, der, der_len);
    if (b64_len <= 0)
    {
      goto done;
    }
    b64[b64_len] = '\0';
  }

  out_csr->csr_base64 = b64;
  b64 = NULL; /* ownership transferred to caller (freed via release_csr) */
  rc = AZ_IOT_OK;

done:
  if (der)
  {
    OPENSSL_free(der);
  }
  if (b64)
  {
    free(b64);
  }
  if (name)
  {
    X509_NAME_free(name);
  }
  if (req)
  {
    X509_REQ_free(req);
  }
  return rc;
}

static void managed_release_csr(
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

/* Write one issued certificate to `b` as PEM.
 *
 * The chain arrives base64-encoded, but what that base64 covers is not fixed:
 * the provisioning service encodes a whole PEM certificate, while the DER form
 * the header documents is what a plain base64-DER encoder produces. Wrapping an
 * already-PEM payload in a second set of BEGIN/END lines yields a file no TLS
 * stack can parse, which surfaces only much later as a handshake failure, so
 * decode first and write whichever form came back.
 *
 * Returns true on success. */
static bool write_issued_cert(BIO* b, const uint8_t* base64, int base64_len)
{
  bool ok = false;
  /* Decoded output is always shorter than its base64; +1 so the result can be
   * examined as a string. */
  unsigned char* decoded = (unsigned char*)calloc(1, (size_t)base64_len + 1);
  if (decoded == NULL)
  {
    return false;
  }

  int decoded_len = EVP_DecodeBlock(decoded, base64, base64_len);
  /* EVP_DecodeBlock writes at most 3 bytes per 4 input bytes; also tells static analysis the
   * reads below stay inside the buffer. */
  if (decoded_len <= 0 || decoded_len > base64_len)
  {
    /* Not decodable base64, so neither form can be recovered from it. Writing it
     * anyway would persist a certificate file that cannot be parsed and would
     * only be discovered later, as a connection failure. */
    free(decoded);
    return false;
  }

  /* EVP_DecodeBlock rounds up to a multiple of three and counts the '=' padding
   * as data, so trim it back before the content is inspected or written. */
  if (base64_len >= 2 && base64[base64_len - 1] == '=')
  {
    decoded_len--;
    if (base64[base64_len - 2] == '=')
    {
      decoded_len--;
    }
  }

  if (decoded_len <= 0)
  {
    free(decoded);
    return false;
  }

  if ((size_t)decoded_len >= strlen(PEM_CERT_PREFIX)
      && memcmp(decoded, PEM_CERT_PREFIX, strlen(PEM_CERT_PREFIX)) == 0)
  {
    /* Already PEM: write it through unchanged, and guarantee the newline that
     * separates it from the next certificate in the chain. */
    ok = BIO_write(b, decoded, decoded_len) == decoded_len;
    if (ok && decoded[decoded_len - 1] != '\n')
    {
      ok = BIO_puts(b, "\n") > 0;
    }
  }
  else
  {
    /* Base64 DER: frame it, breaking the body into PEM-length lines. A single
     * unbroken line is accepted by some parsers and rejected by others. */
    ok = BIO_puts(b, PEM_CERT_BEGIN) > 0;
    for (int off = 0; ok && off < base64_len; off += PEM_LINE_LEN)
    {
      int chunk = base64_len - off < PEM_LINE_LEN ? base64_len - off : PEM_LINE_LEN;
      ok = BIO_write(b, base64 + off, chunk) == chunk;
      if (ok && off + chunk < base64_len)
      {
        ok = BIO_puts(b, "\n") > 0;
      }
    }
    if (ok)
    {
      ok = BIO_puts(b, PEM_CERT_END) > 0;
    }
  }

  free(decoded);
  return ok;
}

static az_iot_result managed_store(
    az_iot_certificate_provider* self,
    const az_iot_issued_certificate* issued)
{
  az_iot_certificate_provider_managed* m = (az_iot_certificate_provider_managed*)self;
  if (!m || !issued)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!m->loaded)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }
  if (!issued->certificates || issued->count == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  BIO* b = BIO_new_file(m->operational_cert_path, "wb");
  if (!b)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  /* Write each issued cert (leaf first) into the operational cert file. */
  az_iot_result rc = AZ_IOT_OK;
  for (size_t i = 0; i < issued->count; ++i)
  {
    az_span cert = issued->certificates[i];
    int len = (int)az_span_size(cert);
    if (len <= 0)
    {
      continue;
    }
    if (!write_issued_cert(b, az_span_ptr(cert), len))
    {
      rc = AZ_IOT_ERR_INTERNAL;
      break;
    }
  }
  BIO_free(b);

  if (rc == AZ_IOT_OK)
  {
    m->has_operational = true;
  }
  return rc;
}

static void managed_deinit_vtable(az_iot_certificate_provider* self)
{
  az_iot_certificate_provider_managed_deinit((az_iot_certificate_provider_managed*)self);
}

static const az_iot_certificate_provider_vtable s_managed_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = managed_load,
  .release = managed_release,
  .deinit = managed_deinit_vtable,
  .get_csr = managed_get_csr,
  .release_csr = managed_release_csr,
  .store_issued_certificate = managed_store,
};

/* --------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

void az_iot_certificate_provider_managed_deinit(az_iot_certificate_provider_managed* provider)
{
  if (!provider)
  {
    return;
  }
  if (provider->operational_key)
  {
    EVP_PKEY_free((EVP_PKEY*)provider->operational_key);
  }
  free(provider->bootstrap_cert_path);
  free(provider->bootstrap_key_path);
  free(provider->trusted_ca_path);
  free(provider->operational_key_path);
  free(provider->operational_cert_path);
  memset(provider, 0, sizeof(*provider));
}

az_iot_result az_iot_certificate_provider_managed_init(
    az_iot_certificate_provider_managed* provider,
    const az_iot_certificate_provider_managed_options* opts)
{
  if (!provider || !opts)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!is_nonempty_cstr(opts->bootstrap_cert_pem_path)
      || !is_nonempty_cstr(opts->bootstrap_key_pem_path)
      || !is_nonempty_cstr(opts->operational_key_pem_path)
      || !is_nonempty_cstr(opts->operational_cert_pem_path))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(provider, 0, sizeof(*provider));
  provider->base.vtable = &s_managed_vtable;
  provider->key_type = (int)opts->key_type;

  provider->bootstrap_cert_path = dup_str(opts->bootstrap_cert_pem_path);
  provider->bootstrap_key_path = dup_str(opts->bootstrap_key_pem_path);
  provider->operational_key_path = dup_str(opts->operational_key_pem_path);
  provider->operational_cert_path = dup_str(opts->operational_cert_pem_path);
  if (!provider->bootstrap_cert_path || !provider->bootstrap_key_path
      || !provider->operational_key_path || !provider->operational_cert_path)
  {
    az_iot_certificate_provider_managed_deinit(provider);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  if (is_nonempty_cstr(opts->trusted_ca_pem_path))
  {
    provider->trusted_ca_path = dup_str(opts->trusted_ca_pem_path);
    if (!provider->trusted_ca_path)
    {
      az_iot_certificate_provider_managed_deinit(provider);
      return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
  }

  /* Operational key: load if present on disk, else generate and persist. */
  EVP_PKEY* key = load_key_file(provider->operational_key_path);
  if (!key)
  {
    key = generate_key(provider->key_type);
    if (!key)
    {
      az_iot_certificate_provider_managed_deinit(provider);
      return AZ_IOT_ERR_INTERNAL;
    }
    az_iot_result wr = write_key_file(provider->operational_key_path, key);
    if (wr != AZ_IOT_OK)
    {
      EVP_PKEY_free(key);
      az_iot_certificate_provider_managed_deinit(provider);
      return wr;
    }
  }
  provider->operational_key = key;

  /* An operational cert persisted by a previous run means we can connect
   * with the OPERATIONAL identity immediately (no re-enrollment needed). */
  provider->has_operational = operational_cert_is_valid(provider->operational_cert_path);

  provider->loaded = true;
  return AZ_IOT_OK;
}
