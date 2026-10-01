// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_sas_signer_symmetric_key.h
 * @brief OpenSSL-backed az_iot_sas_signer holding a symmetric key in memory.
 *
 * Takes an individual enrollment / device key, or an enrollment-group key from
 * which it derives the device key (HMAC-SHA256(group key, registration id)), as
 * DPS does. The same signer can serve both roles: the hub identity DPS creates
 * has the same key.
 *
 * Keep the key in a TPM or HSM instead by implementing az_iot_sas_signer.
 */
#ifndef AZ_IOT_SAS_SIGNER_SYMMETRIC_KEY_H
#define AZ_IOT_SAS_SIGNER_SYMMETRIC_KEY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_sas_signer.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief Options for az_iot_sas_signer_symmetric_key_init(). */
  typedef struct az_iot_sas_signer_symmetric_key_options
  {
    /** @brief Base64 key: the enrollment's or device's key, or the group key
     * when is_enrollment_group_key is set. Required. Copied. */
    const char* symmetric_key_base64;
    /** @brief symmetric_key_base64 is an enrollment-group key; the device key
     * is derived from it and registration_id. */
    bool is_enrollment_group_key;
    /** @brief Required when is_enrollment_group_key is set. Copied. */
    const char* registration_id;
  } az_iot_sas_signer_symmetric_key_options;

  /** @brief Caller-owned signer. Fields are internal. */
  typedef struct az_iot_sas_signer_symmetric_key
  {
    az_iot_sas_signer base; /**< Must be first. */
    uint8_t key[64]; /**< Decoded (or derived) device key. */
    size_t key_len; /**< Bytes used in key. */
  } az_iot_sas_signer_symmetric_key;

  /**
   * @brief Returns options with every field zeroed.
   * @return Default options.
   */
  AZ_NODISCARD az_iot_sas_signer_symmetric_key_options
  az_iot_sas_signer_symmetric_key_options_default(void);

  /**
   * @brief Initializes @p signer.
   *
   * @param[out] signer Signer to initialize.
   * @param[in] opts    Options.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for a missing or undecodable key,
   * or a group key without registration_id.
   */
  AZ_NODISCARD az_iot_result az_iot_sas_signer_symmetric_key_init(
      az_iot_sas_signer_symmetric_key* signer,
      const az_iot_sas_signer_symmetric_key_options* opts);

  /**
   * @brief Wipes the key. Does not free @p signer.
   *
   * @param[in,out] signer Signer to release.
   */
  void az_iot_sas_signer_symmetric_key_deinit(az_iot_sas_signer_symmetric_key* signer);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SAS_SIGNER_SYMMETRIC_KEY_H */
