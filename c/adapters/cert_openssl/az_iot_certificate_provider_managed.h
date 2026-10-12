// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* OpenSSL-backed "managed" certificate provider (design decision D5).
 *
 * A ready-to-use az_iot_certificate_provider for CSR-based enrollment. It:
 *   - authenticates to DPS with a caller-supplied X.509 bootstrap identity,
 *     if any (none: DPS uses SAS from dps_auth);
 *   - owns an operational private key (loaded from disk if present, else
 *     generated on first use and persisted);
 *   - produces PKCS#10 CSRs (get_csr) over the operational key or, with
 *     rotate_key_on_csr_request, over a NEW key held in memory only until a
 *     certificate for it is stored, so every issuance rotates the key;
 *   - persists the DPS/Hub-issued operational certificate chain to disk
 *     (store_issued_certificate) and serves it back on subsequent loads and
 *     process restarts. A store is refused unless every certificate in the
 *     chain parses, the PEM chain is at most 64 KiB and the leaf certifies
 *     the operational key or, with rotation, the pending CSR key (which then
 *     replaces the operational key file). A failed store keeps the previous
 *     key and certificate usable. If the process stops between the chain and
 *     key renames, init() finishes or undoes the rotation from the files it
 *     staged, so the pair matches. A pending key does not survive deinit or a
 *     restart: a chain for it arriving afterwards is refused; request a new
 *     CSR.
 *
 * Files it writes (key and chain) are created readable only by the current user
 * (0600 on POSIX, owner-only DACL on Windows), under a unique temporary name
 * ("<path>.aziot-...") next to the destination, then renamed over it. init()
 * uses such files a stopped process left behind to recover, then deletes them;
 * if recovery cannot complete they are kept for the next init(), and the
 * operational identity is not served until then (if the key cannot be loaded
 * either, no key is generated and get_csr() fails until recovery succeeds).
 * The private key is stored unencrypted: protect the directory accordingly.
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
    /* Bootstrap X.509 identity that authenticates to DPS. Both set for X.509
     * onboarding; both NULL for SAS onboarding (dps_auth), where load() of
     * AZ_IOT_CRED_BOOTSTRAP returns AZ_IOT_ERR_NOT_FOUND. One set and one NULL
     * fails init with AZ_IOT_ERR_INVALID_ARG. */
    const char* bootstrap_cert_pem_path; /* set with key, or NULL with key */
    const char* bootstrap_key_pem_path; /* set with cert, or NULL with cert */
    /* Trusted CA presented to both bootstrap and operational connections. */
    const char* trusted_ca_pem_path; /* may be NULL */
    /* Operational private key. Loaded if the file holds a valid key, which is
     * then left as is (mode included). Otherwise -- missing, unreadable or
     * malformed -- a new key is generated and written here (unencrypted PEM,
     * owner-only), replacing the file. Exception: while a stopped rotation
     * cannot be recovered (see above), no key is generated and only the
     * bootstrap identity is served. Each rotation replaces the file with a new
     * owner-only one holding the new key, so its previous mode or ACL is not
     * kept. Required. */
    const char* operational_key_pem_path; /* required */
    /* Where the issued operational certificate chain is persisted. Written by
     * store_issued_certificate(); read back on load() and on restart, and used
     * only if every certificate parses and the leaf certifies the operational
     * key. Required. */
    const char* operational_cert_pem_path; /* required */
    /* Key type used only when generating a new operational key. */
    az_iot_certificate_managed_key_type key_type;
    /* true: each get_csr() uses a new key, which replaces the operational key
     * once a chain for it is stored. false (default): CSRs are over the
     * operational key, which is never replaced. */
    bool rotate_key_on_csr_request;
  } az_iot_certificate_provider_managed_options;

  /* Returns an options struct with every field defaulted (paths NULL, EC P-256,
   * no rotation). Set the required paths before
   * az_iot_certificate_provider_managed_init(). */
  az_iot_certificate_provider_managed_options az_iot_certificate_provider_managed_options_default(
      void);

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
    void* pending_key; /* EVP_PKEY* of the last CSR, awaiting its chain (opaque) */
    char* served_cert_path; /* previous chain kept aside after a failed rollback; NULL normally */
    bool rotate_key_on_csr_request;
  } az_iot_certificate_provider_managed;

  /* Initialize the managed provider. Loads or generates the operational key and
   * detects any previously-persisted operational certificate. Returns
   * ERR_INVALID_ARG if a required path is missing or a configured path starts
   * with "<operational key or cert path>.aziot-" (that prefix names the
   * provider's temporary files), or ERR_INTERNAL on OpenSSL
   * failure. The provider base pointer can be passed wherever
   * az_iot_certificate_provider* is expected. */
  az_iot_result az_iot_certificate_provider_managed_init(
      az_iot_certificate_provider_managed* provider,
      const az_iot_certificate_provider_managed_options* opts);

  /* Release the operational key and heap-owned paths. Does NOT free the struct
   * itself and does NOT delete any files on disk. */
  void az_iot_certificate_provider_managed_deinit(az_iot_certificate_provider_managed* provider);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CERTIFICATE_PROVIDER_MANAGED_H */
