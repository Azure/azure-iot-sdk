// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* OpenSSL-backed "managed" certificate provider (design decision D5).
 *
 * A ready-to-use az_iot_certificate_provider for CSR-based enrollment. It:
 *   - authenticates to DPS with a caller-supplied X.509 bootstrap identity;
 *   - owns an operational private key (loaded from disk if present, else
 *     generated on first use and persisted);
 *   - produces PKCS#10 CSRs over that operational key (get_csr);
 *   - persists the DPS/Hub-issued operational certificate chain to disk
 *     (store_issued_certificate) and serves it back on subsequent loads and
 *     process restarts.
 *
 * This is the reference implementation of the CSR provider contract. Deployments
 * with a TPM/HSM/secure element should implement their own provider with a
 * non-extractable key (see the sign() hook and client_key_uri in
 * az_iot_certificate_provider.h). Requires OpenSSL 3.0+.
 */
#ifndef AZ_IOT_CERTIFICATE_PROVIDER_MANAGED_H
#define AZ_IOT_CERTIFICATE_PROVIDER_MANAGED_H

#include <stdbool.h>

#include "azure/iot/az_iot_certificate_provider.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* Key type used when a new operational key must be generated. */
  typedef enum
  {
    AZ_IOT_MANAGED_KEY_EC_P256 = 0, /* default: ECDSA P-256 */
    AZ_IOT_MANAGED_KEY_RSA_2048 /* RSA 2048 */
  } az_iot_certificate_managed_key_type;

  typedef struct az_iot_certificate_provider_managed_options
  {
    /* Bootstrap X.509 identity that authenticates to DPS. Both required. */
    const char* bootstrap_cert_pem_path; /* required */
    const char* bootstrap_key_pem_path; /* required */
    /* Trusted CA presented to both bootstrap and operational connections. */
    const char* trusted_ca_pem_path; /* may be NULL */
    /* Operational private key. Loaded if the file exists, otherwise a new key
     * is generated and written here (PEM). Required. */
    const char* operational_key_pem_path; /* required */
    /* Where the issued operational certificate chain is persisted. Written by
     * store_issued_certificate(); read back on load() and on restart. Required. */
    const char* operational_cert_pem_path; /* required */
    /* Key type used only when generating a new operational key. */
    az_iot_certificate_managed_key_type key_type;
  } az_iot_certificate_provider_managed_options;

  /* Caller-owned managed certificate provider struct. Fields are INTERNAL. */
  typedef struct az_iot_certificate_provider_managed
  {
    az_iot_certificate_provider base; /* MUST be first (vtable pointer) */
    char* bootstrap_cert_path;
    char* bootstrap_key_path;
    char* trusted_ca_path;
    char* operational_key_path;
    char* operational_cert_path;
    void* operational_key; /* EVP_PKEY* (opaque) */
    int key_type;
    bool has_operational; /* issued cert present on disk */
    bool loaded;
  } az_iot_certificate_provider_managed;

  /* Initialize the managed provider. Loads or generates the operational key and
   * detects any previously-persisted operational certificate. Returns
   * ERR_INVALID_ARG if a required path is missing, or ERR_INTERNAL on OpenSSL
   * failure. The provider base pointer can be passed wherever
   * az_iot_certificate_provider* is expected. */
  az_iot_result az_iot_certificate_provider_managed_init(
      az_iot_certificate_provider_managed* provider,
      const az_iot_certificate_provider_managed_options* opts);

  /* Release the operational key and heap-owned paths. Does NOT free the struct
   * itself and does NOT delete any files on disk. */
  void az_iot_certificate_provider_managed_destroy(az_iot_certificate_provider_managed* provider);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CERTIFICATE_PROVIDER_MANAGED_H */
