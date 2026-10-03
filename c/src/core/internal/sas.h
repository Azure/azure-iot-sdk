// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file sas.h
 * @brief SAS token construction for DPS and the mqttv3 hub.
 */
#ifndef AZ_IOT_SAS_INTERNAL_H
#define AZ_IOT_SAS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_crypto.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Zeroes @p len bytes at @p p; the compiler may not elide it.
   *
   * @param[out] p   Memory to clear; may be NULL when @p len is 0.
   * @param[in]  len Bytes to clear.
   */
  void az_iot_sas__wipe(void* p, size_t len);

  /**
   * @brief Decodes a base64 symmetric key.
   *
   * @param[in]  key_base64 NUL-terminated base64 key.
   * @param[out] out        Decoded key; wiped on failure.
   * @param[in]  cap        Bytes in @p out.
   * @param[out] out_len    Bytes written to @p out.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG when the key is empty, not
   * base64, or decodes to more than @p cap bytes.
   */
  AZ_NODISCARD az_iot_result
  az_iot_sas__decode_key(const char* key_base64, uint8_t* out, size_t cap, size_t* out_len);

  /**
   * @brief Derives a device key from an enrollment-group key:
   * HMAC-SHA256(group key, @p id).
   *
   * @param[in]  crypto        Validated backend.
   * @param[in]  group_key     Decoded group key.
   * @param[in]  group_key_len Bytes in @p group_key.
   * @param[in]  id            NUL-terminated registration or device ID.
   * @param[out] out           Device key.
   * @return AZ_IOT_OK, or the backend's error.
   */
  AZ_NODISCARD az_iot_result az_iot_sas__derive_device_key(
      const az_iot_crypto* crypto,
      const uint8_t* group_key,
      size_t group_key_len,
      const char* id,
      uint8_t out[AZ_IOT_SHA256_SIZE]);

  /**
   * @brief Builds the URL-encoded resource URI (the token's `sr` value).
   *
   * DPS: `<id_scope>%2fregistrations%2f<registration_id>`. Hub:
   * `<host>%2Fdevices%2F<device_id>`. Each ID is URL-encoded.
   *
   * @param[in]  is_dps  DPS form when true, hub form otherwise.
   * @param[in]  first   ID scope (DPS) or hub host name.
   * @param[in]  second  Registration ID (DPS) or device ID.
   * @param[out] out     NUL-terminated result.
   * @param[in]  cap     Bytes in @p out.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for an empty ID;
   * AZ_IOT_ERR_NOT_ENOUGH_SPACE when @p out is too small.
   */
  AZ_NODISCARD az_iot_result az_iot_sas__resource_uri(
      bool is_dps,
      const char* first,
      const char* second,
      char* out,
      size_t cap);

  /**
   * @brief Signs `<resource_uri>\n<expiry>` and formats
   * `SharedAccessSignature sr=<uri>&sig=<sig>&se=<expiry>[&skn=<key_name>]`.
   *
   * @param[in]  crypto       Validated backend.
   * @param[in]  key          Decoded signing key.
   * @param[in]  key_len      Bytes in @p key.
   * @param[in]  resource_uri NUL-terminated, already URL-encoded.
   * @param[in]  key_name     NUL-terminated `skn` value; "" for none.
   * @param[in]  expiry       Expiry, seconds since the Unix epoch.
   * @param[out] out          NUL-terminated token; wiped on failure.
   * @param[in]  cap          Bytes in @p out.
   * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_ENOUGH_SPACE when @p out is too small;
   * the backend's error when signing fails.
   */
  AZ_NODISCARD az_iot_result az_iot_sas__build_token(
      const az_iot_crypto* crypto,
      const uint8_t* key,
      size_t key_len,
      const char* resource_uri,
      const char* key_name,
      uint64_t expiry,
      char* out,
      size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SAS_INTERNAL_H */
