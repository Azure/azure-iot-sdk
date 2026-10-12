// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* CSR backend used by sample_cert_provider (OpenSSL 3.0+: sample_csr_openssl.c). */
#ifndef SAMPLE_CSR_BACKEND_H
#define SAMPLE_CSR_BACKEND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /* Ensure an operational private key exists for key_path - loading it when
   * present, otherwise generating an EC P-256 key - and keep it as PEM at
   * key_path; then produce a PKCS#10 CSR over that key with the given subject
   * common name. Every call for the same key_path uses the same key. On success
   * writes a heap-owned base64 DER CSR (no PEM header/newlines, matching the
   * DPS/Hub wire format) to *out_csr (caller frees with free()) and returns 0.
   * Returns non-zero on failure. */
  int sample_csr_backend_get_csr(const char* key_path, const char* subject_cn, char** out_csr);

  /* Returns 1 when @p cert_base64 (base64 of exactly one DER or PEM
   * certificate, @p len bytes, at most 64 KiB) certifies the private key at
   * key_path, else 0. Never creates a key. */
  int sample_csr_backend_cert_matches_key(
      const char* key_path,
      const char* cert_base64,
      size_t len);

  /* Returns 1 when the first certificate in the PEM file at cert_path certifies
   * the private key at key_path, else 0. */
  int sample_csr_backend_cert_file_matches_key(const char* key_path, const char* cert_path);

#ifdef __cplusplus
}
#endif

#endif /* SAMPLE_CSR_BACKEND_H */
