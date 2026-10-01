// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_certificate_provider_symmetric_key.h
 * @brief OpenSSL-backed az_iot_certificate_provider for symmetric-key (SAS)
 * authentication.
 *
 * - load() returns AZ_IOT_CREDENTIAL_SAS with the trusted CA only.
 * - sign_sas() computes HMAC-SHA256 with an in-memory key.
 * - An enrollment-group key is turned into the device key
 *   (HMAC-SHA256(group key, registration id)), as DPS does.
 * - Optionally delegates the OPERATIONAL role, CSR and certificate storage to
 *   an X.509 provider, so DPS can issue the hub certificate.
 *
 * The key is held in process memory. Keep it in a TPM or HSM instead by
 * implementing sign_sas() in your own provider.
 */
#ifndef AZ_IOT_CERTIFICATE_PROVIDER_SYMMETRIC_KEY_H
#define AZ_IOT_CERTIFICATE_PROVIDER_SYMMETRIC_KEY_H

#include <stdbool.h>
#include <stdint.h>

#include "azure/iot/az_iot_certificate_provider.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief Options for az_iot_certificate_provider_symmetric_key_init(). */
  typedef struct az_iot_certificate_provider_symmetric_key_options
  {
    /** @brief Base64 key: the enrollment's or device's key, or the group key
     * when is_enrollment_group_key is set. Required. Copied. */
    const char* symmetric_key_base64;
    /** @brief symmetric_key_base64 is an enrollment-group key; the device key
     * is derived from it and registration_id. */
    bool is_enrollment_group_key;
    /** @brief Required when is_enrollment_group_key is set. Copied. */
    const char* registration_id;
    /** @brief Trusted CA PEM path. NULL selects the adapter's default store. */
    const char* trusted_ca_pem_path;
    /**
     * @brief X.509 provider for the OPERATIONAL role, or NULL.
     *
     * NULL: SAS for both DPS and hub (the hub identity DPS creates has the
     * same key). Set: the hub uses this provider's operational certificate,
     * and get_csr / release_csr / store_issued_certificate / sign forward to
     * it. Borrowed; must outlive this provider.
     */
    az_iot_certificate_provider* operational;
  } az_iot_certificate_provider_symmetric_key_options;

  /** @brief Caller-owned provider. Fields are internal. */
  typedef struct az_iot_certificate_provider_symmetric_key
  {
    az_iot_certificate_provider base; /**< Must be first. */
    uint8_t key[64]; /**< Decoded (or derived) device key. */
    size_t key_len; /**< Bytes used in key. */
    char* trusted_ca_path; /**< Owned copy, or NULL. */
    az_iot_certificate_provider* operational; /**< Borrowed delegate, or NULL. */
  } az_iot_certificate_provider_symmetric_key;

  /**
   * @brief Returns options with every field zeroed.
   * @return Default options.
   */
  AZ_NODISCARD az_iot_certificate_provider_symmetric_key_options
  az_iot_certificate_provider_symmetric_key_options_default(void);

  /**
   * @brief Initializes @p provider.
   *
   * @param[out] provider Provider to initialize.
   * @param[in] opts      Options.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for a missing or undecodable key,
   * or a group key without registration_id.
   */
  AZ_NODISCARD az_iot_result az_iot_certificate_provider_symmetric_key_init(
      az_iot_certificate_provider_symmetric_key* provider,
      const az_iot_certificate_provider_symmetric_key_options* opts);

  /**
   * @brief Wipes the key and frees owned memory. Does not free @p provider or
   * deinit the operational delegate.
   *
   * @param[in,out] provider Provider to release.
   */
  void az_iot_certificate_provider_symmetric_key_deinit(
      az_iot_certificate_provider_symmetric_key* provider);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CERTIFICATE_PROVIDER_SYMMETRIC_KEY_H */
