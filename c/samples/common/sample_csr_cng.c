// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Windows CNG/NCrypt CSR backend for sample_cert_provider. Keeps one EC P-256
 * operational key per key_path as a persisted CNG key (named from key_path),
 * creating it on first use, produces a PKCS#10 CSR over it via
 * CryptSignAndEncodeCertificate(X509_CERT_REQUEST_TO_BE_SIGNED), and exports it
 * as a PKCS#8 PEM at key_path so the (OpenSSL-based) Paho TLS layer can load it.
 * Every call uses the same key; a new key_path gives a new key.
 *
 * Link: crypt32.lib, ncrypt.lib. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS /* fopen for writing the exported key PEM */
#endif
#define WIN32_LEAN_AND_MEAN

#include "sample_csr_backend.h"

#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <ncrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Base64-encode DER into a heap string. When no_crlf is set the output is a
 * single line (wire CSR); otherwise it is CRLF-wrapped for a PEM body. Returns
 * NULL on failure. */
static char* base64_dup(const BYTE* der, DWORD der_len, BOOL no_crlf)
{
  DWORD flags = CRYPT_STRING_BASE64 | (no_crlf ? CRYPT_STRING_NOCRLF : 0);
  DWORD cch = 0;
  if (!CryptBinaryToStringA(der, der_len, flags, NULL, &cch) || cch == 0)
  {
    return NULL;
  }
  char* out = (char*)malloc(cch);
  if (!out)
  {
    return NULL;
  }
  if (!CryptBinaryToStringA(der, der_len, flags, out, &cch))
  {
    free(out);
    return NULL;
  }
  return out;
}

/* Export the NCrypt private key as a PKCS#8 PEM file at path. Returns 0 on ok. */
/* Name of the persisted CNG key for @p key_path: a fixed prefix and a 64-bit
 * FNV-1a hash of the path, case-folded (Windows paths are case-insensitive). */
static void persisted_key_name(const char* key_path, WCHAR out[64])
{
  unsigned long long h = 1469598103934665603ULL;
  for (const char* c = key_path; *c; ++c)
  {
    char ch = (*c >= 'A' && *c <= 'Z') ? (char)(*c - 'A' + 'a') : *c;
    h = (h ^ (unsigned char)ch) * 1099511628211ULL;
  }
  (void)_snwprintf_s(out, 64, _TRUNCATE, L"azure-iot-sample-%016llx", h);
}

static int export_key_pem(NCRYPT_KEY_HANDLE hKey, const char* path)
{
  int rc = 1;
  DWORD cb = 0;
  BYTE* blob = NULL;
  char* b64 = NULL;
  FILE* f = NULL;

  if (NCryptExportKey(hKey, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, NULL, 0, &cb, 0)
          != ERROR_SUCCESS
      || cb == 0)
  {
    goto done;
  }
  blob = (BYTE*)malloc(cb);
  if (!blob)
  {
    goto done;
  }
  if (NCryptExportKey(hKey, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, NULL, blob, cb, &cb, 0)
      != ERROR_SUCCESS)
  {
    goto done;
  }

  b64 = base64_dup(blob, cb, FALSE); /* CRLF-wrapped PEM body */
  if (!b64)
  {
    goto done;
  }

  f = fopen(path, "wb");
  if (!f)
  {
    goto done;
  }
  if (fputs("-----BEGIN PRIVATE KEY-----\r\n", f) < 0)
  {
    goto done;
  }
  if (fputs(b64, f) < 0)
  {
    goto done;
  }
  if (fputs("-----END PRIVATE KEY-----\r\n", f) < 0)
  {
    goto done;
  }
  rc = 0;

done:
  /* A deferred write error surfaces only on flush or close. */
  if (f)
  {
    int flushed = fflush(f);
    if (fclose(f) != 0 || flushed != 0)
    {
      rc = 1;
    }
    if (rc != 0)
    {
      (void)remove(path); /* never leave a partial key behind */
    }
  }
  if (b64)
  {
    free(b64);
  }
  if (blob)
  {
    free(blob);
  }
  return rc;
}

int sample_csr_backend_get_csr(const char* key_path, const char* subject_cn, char** out_csr)
{
  if (!key_path || !out_csr)
  {
    return 1;
  }
  *out_csr = NULL;

  int rc = 1;
  NCRYPT_PROV_HANDLE hProv = 0;
  NCRYPT_KEY_HANDLE hKey = 0;
  BYTE* name_blob = NULL;
  CERT_PUBLIC_KEY_INFO* pubkey = NULL;
  BYTE* csr_der = NULL;
  char* csr_b64 = NULL;

  const char* cn = (subject_cn && subject_cn[0]) ? subject_cn : "azure-iot-device";
  char subject[256];
  if (_snprintf_s(subject, sizeof(subject), _TRUNCATE, "CN=%s", cn) < 0)
  {
    return 1;
  }

  /* 1. Open the persisted EC P-256 key for key_path, creating it (exportable)
   *    on first use. */
  if (NCryptOpenStorageProvider(&hProv, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS)
  {
    goto done;
  }
  {
    WCHAR key_name[64];
    persisted_key_name(key_path, key_name);
    SECURITY_STATUS st = NCryptOpenKey(hProv, &hKey, key_name, 0, 0);
    if (st == NTE_BAD_KEYSET)
    {
      hKey = 0;
      DWORD policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
      if (NCryptCreatePersistedKey(hProv, &hKey, BCRYPT_ECDSA_P256_ALGORITHM, key_name, 0, 0)
              != ERROR_SUCCESS
          || NCryptSetProperty(
                 hKey, NCRYPT_EXPORT_POLICY_PROPERTY, (PBYTE)&policy, sizeof(policy), 0)
              != ERROR_SUCCESS
          || NCryptFinalizeKey(hKey, 0) != ERROR_SUCCESS)
      {
        goto done;
      }
    }
    else if (st != ERROR_SUCCESS)
    {
      hKey = 0;
      goto done;
    }
  }

  /* 2. Encode the subject DN. */
  {
    DWORD cb = 0;
    if (!CertStrToNameA(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, NULL, NULL, &cb, NULL)
        || cb == 0)
    {
      goto done;
    }
    name_blob = (BYTE*)malloc(cb);
    if (!name_blob)
    {
      goto done;
    }
    if (!CertStrToNameA(X509_ASN_ENCODING, subject, CERT_X500_NAME_STR, NULL, name_blob, &cb, NULL))
    {
      goto done;
    }

    /* 3. Export the SubjectPublicKeyInfo. */
    DWORD cbPub = 0;
    if (!CryptExportPublicKeyInfo(hKey, 0, X509_ASN_ENCODING, NULL, &cbPub) || cbPub == 0)
    {
      goto done;
    }
    pubkey = (CERT_PUBLIC_KEY_INFO*)malloc(cbPub);
    if (!pubkey)
    {
      goto done;
    }
    if (!CryptExportPublicKeyInfo(hKey, 0, X509_ASN_ENCODING, pubkey, &cbPub))
    {
      goto done;
    }

    /* 4. Sign+encode the PKCS#10 request (ECDSA/SHA-256). */
    CERT_REQUEST_INFO req;
    memset(&req, 0, sizeof(req));
    req.dwVersion = CERT_REQUEST_V1;
    req.Subject.cbData = cb;
    req.Subject.pbData = name_blob;
    req.SubjectPublicKeyInfo = *pubkey;

    CRYPT_ALGORITHM_IDENTIFIER sig;
    memset(&sig, 0, sizeof(sig));
    sig.pszObjId = (LPSTR)szOID_ECDSA_SHA256;

    DWORD cbCsr = 0;
    if (!CryptSignAndEncodeCertificate(
            hKey,
            0,
            X509_ASN_ENCODING,
            X509_CERT_REQUEST_TO_BE_SIGNED,
            &req,
            &sig,
            NULL,
            NULL,
            &cbCsr)
        || cbCsr == 0)
    {
      goto done;
    }
    csr_der = (BYTE*)malloc(cbCsr);
    if (!csr_der)
    {
      goto done;
    }
    if (!CryptSignAndEncodeCertificate(
            hKey,
            0,
            X509_ASN_ENCODING,
            X509_CERT_REQUEST_TO_BE_SIGNED,
            &req,
            &sig,
            NULL,
            csr_der,
            &cbCsr))
    {
      goto done;
    }

    /* 5. Base64 the CSR (single line, wire format). */
    csr_b64 = base64_dup(csr_der, cbCsr, TRUE);
    if (!csr_b64)
    {
      goto done;
    }
  }

  /* 6. Write the private key as PEM for the TLS stack: to a temporary file,
   *    then over key_path in one step. It is the same key every time. */
  {
    char tmp[MAX_PATH];
    if (_snprintf_s(tmp, sizeof(tmp), _TRUNCATE, "%s.tmp", key_path) < 0
        || export_key_pem(hKey, tmp) != 0)
    {
      goto done;
    }
    if (!MoveFileExA(tmp, key_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
      (void)remove(tmp);
      goto done;
    }
  }

  *out_csr = csr_b64;
  csr_b64 = NULL;
  rc = 0;

done:
  if (csr_b64)
  {
    free(csr_b64);
  }
  if (csr_der)
  {
    free(csr_der);
  }
  if (pubkey)
  {
    free(pubkey);
  }
  if (name_blob)
  {
    free(name_blob);
  }
  if (hKey)
  {
    NCryptFreeObject(hKey);
  }
  if (hProv)
  {
    NCryptFreeObject(hProv);
  }
  return rc;
}
