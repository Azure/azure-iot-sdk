// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Platform CSR backend used by sample_cert_provider. Two implementations exist,
 * selected at build time: sample_csr_openssl.c (non-Windows) and
 * sample_csr_cng.c (Windows). */
#ifndef SAMPLE_CSR_BACKEND_H
#define SAMPLE_CSR_BACKEND_H

#ifdef __cplusplus
extern "C"
{
#endif

  /* Ensure an operational private key exists at key_path - loading it when the
   * PEM file is present, otherwise generating an EC P-256 key and persisting it as
   * PEM - then produce a PKCS#10 CSR over that key with the given subject common
   * name. On success writes a heap-owned base64 DER CSR (no PEM header/newlines,
   * matching the DPS/Hub wire format) to *out_csr (caller frees with free()) and
   * returns 0. Returns non-zero on failure. */
  int sample_csr_backend_get_csr(const char* key_path, const char* subject_cn, char** out_csr);

#ifdef __cplusplus
}
#endif

#endif /* SAMPLE_CSR_BACKEND_H */
