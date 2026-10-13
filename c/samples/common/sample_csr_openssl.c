// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* OpenSSL 3.0+ CSR backend for sample_cert_provider. Loads or
 * generates an EC P-256 operational key (persisted as PEM) and produces a
 * PKCS#10 CSR over it. Uses only public OpenSSL 3.0 EVP/X509_REQ/BIO APIs. */
#include "sample_csr_backend.h"

#include <stdlib.h>
#include <string.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

static EVP_PKEY* load_or_generate_key(const char* key_path)
{
  BIO* b = BIO_new_file(key_path, "rb");
  if (b)
  {
    EVP_PKEY* k = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
    BIO_free(b);
    if (k)
    {
      return k;
    }
    ERR_clear_error();
  }
  else
  {
    ERR_clear_error();
  }

  EVP_PKEY* key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  if (!key)
  {
    return NULL;
  }

  BIO* out = BIO_new_file(key_path, "wb");
  if (!out)
  {
    EVP_PKEY_free(key);
    return NULL;
  }
  int ok = PEM_write_bio_PrivateKey(out, key, NULL, NULL, 0, NULL, NULL);
  BIO_free(out);
  if (ok != 1)
  {
    EVP_PKEY_free(key);
    return NULL;
  }
  return key;
}

int sample_csr_backend_get_csr(const char* key_path, const char* subject_cn, char** out_csr)
{
  if (!key_path || !out_csr)
  {
    return 1;
  }
  *out_csr = NULL;

  EVP_PKEY* key = load_or_generate_key(key_path);
  if (!key)
  {
    return 1;
  }

  X509_REQ* req = NULL;
  X509_NAME* name = NULL;
  unsigned char* der = NULL;
  char* b64 = NULL;
  int rc = 1;

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
    const char* cn = (subject_cn && subject_cn[0]) ? subject_cn : "azure-iot-device";
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

    size_t cap = (size_t)(((der_len + 2) / 3) * 4) + 1;
    b64 = (char*)malloc(cap);
    if (!b64)
    {
      goto done;
    }
    int b64_len = EVP_EncodeBlock((unsigned char*)b64, der, der_len);
    if (b64_len <= 0)
    {
      goto done;
    }
    b64[b64_len] = '\0';
  }

  *out_csr = b64;
  b64 = NULL;
  rc = 0;

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
  EVP_PKEY_free(key);
  return rc;
}

/* True when only whitespace remains in memory BIO @p b. */
static int only_whitespace_left(BIO* b)
{
  char* rest = NULL;
  long n = BIO_get_mem_data(b, &rest);
  for (long i = 0; i < n; ++i)
  {
    if (rest[i] != ' ' && rest[i] != '\t' && rest[i] != '\r' && rest[i] != '\n')
    {
      return 0;
    }
  }
  return 1;
}

/* Exactly one certificate from a wire entry: base64 of DER or of a PEM
 * certificate. NULL when malformed, or when anything but whitespace follows. */
static X509* parse_cert_entry(const char* cert_base64, size_t len)
{
  if (!cert_base64 || len == 0 || len > (size_t)64u * 1024u)
  {
    return NULL;
  }
  X509* cert = NULL;
  unsigned char* der = (unsigned char*)malloc((len / 4u) * 3u + 3u);
  int der_len = der ? EVP_DecodeBlock(der, (const unsigned char*)cert_base64, (int)len) : -1;
  if (der_len > 0)
  {
    /* EVP_DecodeBlock counts '=' padding as zero bytes. */
    for (size_t i = len; i > 0 && cert_base64[i - 1] == '=' && der_len > 0; --i)
    {
      der_len--;
    }
    /* "LS0tLS1CRUdJTi" is base64 of "-----BEGIN": a PEM certificate. */
    static const char pem_b64[] = "LS0tLS1CRUdJTi";
    if (len >= sizeof(pem_b64) - 1 && memcmp(cert_base64, pem_b64, sizeof(pem_b64) - 1) == 0)
    {
      BIO* b = BIO_new_mem_buf(der, der_len);
      cert = b ? PEM_read_bio_X509(b, NULL, NULL, NULL) : NULL;
      if (cert && !only_whitespace_left(b))
      {
        X509_free(cert);
        cert = NULL;
      }
      BIO_free(b);
    }
    else
    {
      const unsigned char* q = der;
      cert = d2i_X509(NULL, &q, der_len);
      if (cert && q != der + der_len)
      {
        X509_free(cert);
        cert = NULL;
      }
    }
  }
  free(der);
  ERR_clear_error();
  return cert;
}

/* True when @p cert certifies the private key at @p key_path. Frees @p cert. */
static int cert_certifies_key_file(X509* cert, const char* key_path)
{
  BIO* kb = cert ? BIO_new_file(key_path, "rb") : NULL;
  EVP_PKEY* key = kb ? PEM_read_bio_PrivateKey(kb, NULL, NULL, NULL) : NULL;
  BIO_free(kb);
  int match = (cert && key && X509_check_private_key(cert, key) == 1) ? 1 : 0;
  EVP_PKEY_free(key);
  X509_free(cert);
  ERR_clear_error();
  return match;
}

int sample_csr_backend_cert_matches_key(const char* key_path, const char* cert_base64, size_t len)
{
  return key_path ? cert_certifies_key_file(parse_cert_entry(cert_base64, len), key_path) : 0;
}

int sample_csr_backend_is_cert(const char* cert_base64, size_t len)
{
  X509* cert = parse_cert_entry(cert_base64, len);
  X509_free(cert);
  return cert != NULL;
}

int sample_csr_backend_cert_file_matches_key(const char* key_path, const char* cert_path)
{
  if (!key_path || !cert_path)
  {
    return 0;
  }
  BIO* cb = BIO_new_file(cert_path, "rb");
  X509* leaf = cb ? PEM_read_bio_X509(cb, NULL, NULL, NULL) : NULL;
  BIO_free(cb);
  return cert_certifies_key_file(leaf, key_path);
}
