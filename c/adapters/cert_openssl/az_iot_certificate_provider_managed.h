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
 *   - produces PKCS#10 CSRs over that operational key (get_csr);
 *   - persists the DPS/Hub-issued operational certificate chain to disk
 *     (store_issued_certificate) and serves it back on subsequent loads and
 *     process restarts. A store is refused unless every certificate in the
 *     chain parses, the leaf certifies the operational key and the PEM chain
 *     is at most 64 KiB; it replaces the file in one step, so a failed store
 *     keeps the previous certificate.
 *
 * The operational key never changes while the provider is initialized. To
 * rotate it: close the connection client, deinit the provider, delete (or point
 * the options at new) key and certificate files, init it again, then open the
 * client and enroll or renew.
 *
 * Files it writes (key and chain) are created readable only by the current user
 * (0600 on POSIX, owner-only DACL on Windows), under a unique temporary name
 * ("<path>.aziot-...") next to the destination, flushed, then renamed over it.
 * The rename is flushed too (parent directory fsync on POSIX, write-through
 * on Windows), so a stored chain survives a power loss. If only that directory
 * sync fails, the write still succeeds: the new file is already in place and
 * served. init() deletes temporary files a stopped process left behind (best
 * effort: one it cannot list or delete is left, and init() continues). The key
 * and chain paths must be used by one provider instance at a time.
 *
 * By design, the operational private key is stored unencrypted (plain PEM),
 * protected only by the file permissions above: a passphrase would have to be
 * stored on the same device. Protect the directory accordingly. Devices that
 * need key protection should keep the key in an HSM/TPM/secure element instead
 * (PKCS#11 via client_key_uri, a PSA Crypto key via client_key_psa_id, or the
 * sign() hook; see az_iot_certificate_provider.h) through their own provider.
 *
 * This is the reference implementation of the CSR provider contract. Requires
 * OpenSSL 3.0+.
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

  /**
   * @brief Key type used when a new operational key must be generated.
   *
   * The CSR digest follows the operational key actually in use (generated or
   * loaded). EC: SHA-256 up to 256 bits, SHA-384 up to 384 bits, SHA-512
   * above. RSA: SHA-256.
   */
  typedef enum
  {
    AZ_IOT_MANAGED_KEY_EC_P256 = 0, /**< Default. ECDSA P-256, CSR signed with SHA-256. */
    AZ_IOT_MANAGED_KEY_RSA_2048, /**< RSA 2048, CSR signed with SHA-256. */
    AZ_IOT_MANAGED_KEY_EC_P384 /**< ECDSA P-384 (secp384r1), CSR signed with SHA-384. */
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
     * owner-only), replacing the file. Required. */
    const char* operational_key_pem_path; /* required */
    /* Where the issued operational certificate chain is persisted. Written by
     * store_issued_certificate(); read back on load() and on restart, and used
     * only if every certificate parses and the leaf certifies the operational
     * key. Required. */
    const char* operational_cert_pem_path; /* required */
    /* Key type used only when generating a new operational key; a loaded key
     * is kept whatever its type. A value outside the enum fails init with
     * AZ_IOT_ERR_INVALID_ARG. */
    az_iot_certificate_managed_key_type key_type;
  } az_iot_certificate_provider_managed_options;

  /* Returns an options struct with every field defaulted (paths NULL, EC P-256).
   * Set the required paths before az_iot_certificate_provider_managed_init(). */
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
  } az_iot_certificate_provider_managed;

  /* Initialize the managed provider. Loads or generates the operational key and
   * detects any previously-persisted operational certificate. Returns
   * ERR_INVALID_ARG if a required path is missing, key_type is unknown, an
   * operational path does not end in a file name (ends in a separator, ".", or
   * ".."), or a configured path starts with
   * "<operational key or cert path>.aziot-" (the temporary-file names), or
   * ERR_INTERNAL on OpenSSL failure. The provider base pointer can be passed
   * wherever az_iot_certificate_provider* is expected. */
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
