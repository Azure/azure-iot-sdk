// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* OpenSSL 3.0+ CSR backend for sample_cert_provider (non-Windows). Loads or
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
      return k;
    ERR_clear_error();
  }
  else
  {
    ERR_clear_error();
  }

  EVP_PKEY* key = EVP_PKEY_Q_keygen(NULL, NULL, "EC", "P-256");
  if (!key)
    return NULL;

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
    return 1;
  *out_csr = NULL;

  EVP_PKEY* key = load_or_generate_key(key_path);
  if (!key)
    return 1;

  X509_REQ* req = NULL;
  X509_NAME* name = NULL;
  unsigned char* der = NULL;
  char* b64 = NULL;
  int rc = 1;

  req = X509_REQ_new();
  if (!req)
    goto done;
  if (X509_REQ_set_version(req, 0L) != 1)
    goto done; /* PKCS#10 v1 */

  name = X509_NAME_new();
  if (!name)
    goto done;
  {
    const char* cn = (subject_cn && subject_cn[0]) ? subject_cn : "azure-iot-device";
    if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_UTF8, (const unsigned char*)cn, -1, -1, 0)
        != 1)
      goto done;
  }
  if (X509_REQ_set_subject_name(req, name) != 1)
    goto done;
  if (X509_REQ_set_pubkey(req, key) != 1)
    goto done;
  if (X509_REQ_sign(req, key, EVP_sha256()) == 0)
    goto done;

  {
    int der_len = i2d_X509_REQ(req, &der);
    if (der_len <= 0 || der == NULL)
      goto done;

    size_t cap = (size_t)(((der_len + 2) / 3) * 4) + 1;
    b64 = (char*)malloc(cap);
    if (!b64)
      goto done;
    int b64_len = EVP_EncodeBlock((unsigned char*)b64, der, der_len);
    if (b64_len <= 0)
      goto done;
    b64[b64_len] = '\0';
  }

  *out_csr = b64;
  b64 = NULL;
  rc = 0;

done:
  if (der)
    OPENSSL_free(der);
  if (b64)
    free(b64);
  if (name)
    X509_NAME_free(name);
  if (req)
    X509_REQ_free(req);
  EVP_PKEY_free(key);
  return rc;
}
