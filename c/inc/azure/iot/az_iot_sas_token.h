// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file az_iot_sas_token.h
 * @brief Builds the SAS tokens supplied with
 * az_iot_connection_client_update_sas_token().
 *
 * A token is `SharedAccessSignature sr=<resource URI>&sig=<signature>&se=<expiry>`,
 * plus `&skn=<key name>` when there is one; the signature is the URL-encoded
 * base64 of HMAC-SHA256(device key, string to sign). Take the resource URI and
 * key name from az_iot_sas_token_request. A signer that keeps the key (HSM,
 * TPM, secure element) computes the HMAC of
 * az_iot_sas_token_string_to_sign() and passes it to
 * az_iot_sas_token_from_signature(); with the key in memory,
 * az_iot_sas_token_sign() does both.
 */

#ifndef AZ_IOT_SAS_TOKEN_H
#define AZ_IOT_SAS_TOKEN_H

#include <stddef.h>
#include <stdint.h>

#include "az_iot_crypto.h"
#include "az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Bytes a token takes, terminator included, at most: for a resource
 * URI of @p uri_len and a key name of @p key_name_len characters.
 */
#define AZ_IOT_SAS_TOKEN_SIZE_FOR(uri_len, key_name_len) \
  ((size_t)(uri_len) + (size_t)(key_name_len) + 192u)

/**
 * @brief Bytes the string to sign takes, terminator included, at most: for a
 * resource URI of @p uri_len characters.
 */
#define AZ_IOT_SAS_STRING_TO_SIGN_SIZE(uri_len) ((size_t)(uri_len) + 22u)

  /**
   * @brief Writes the string a token's signature is computed over:
   * `<resource URI>\n<expiry>`.
   *
   * @param[in]  resource_uri         URL-encoded resource URI
   *                                  (az_iot_sas_token_request::resource_uri).
   * @param[in]  expiry_unix_seconds  Expiry, in seconds since the Unix epoch; not 0.
   * @param[out] out                  Buffer; NUL-terminated on success, zeroed
   *                                  on failure.
   * @param[in]  out_size             Bytes in @p out; see AZ_IOT_SAS_STRING_TO_SIGN_SIZE().
   * @param[out] out_len              Characters written, terminator excluded.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for a NULL pointer, an empty
   * resource URI or one with characters other than `A-Z a-z 0-9 - . _ ~ %`, or
   * a 0 expiry; AZ_IOT_ERR_NOT_ENOUGH_SPACE when @p out is too small.
   */
  AZ_NODISCARD az_iot_result az_iot_sas_token_string_to_sign(
      const char* resource_uri,
      uint64_t expiry_unix_seconds,
      char* out,
      size_t out_size,
      size_t* out_len);

  /**
   * @brief Writes a token from the HMAC-SHA256 of its string to sign.
   *
   * @param[in]  resource_uri         As for az_iot_sas_token_string_to_sign().
   * @param[in]  key_name             az_iot_sas_token_request::key_name; NULL
   *                                  or empty for none. Same characters as
   *                                  @p resource_uri.
   * @param[in]  expiry_unix_seconds  The expiry that was signed; not 0.
   * @param[in]  signature            HMAC-SHA256 of the string to sign.
   * @param[out] out                  Buffer; NUL-terminated on success, zeroed
   *                                  on failure.
   * @param[in]  out_size             Bytes in @p out; see AZ_IOT_SAS_TOKEN_SIZE_FOR().
   * @param[out] out_len              Characters written, terminator excluded.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG as for
   * az_iot_sas_token_string_to_sign(), or for a NULL @p signature or a bad
   * key name; AZ_IOT_ERR_NOT_ENOUGH_SPACE when @p out is too small.
   */
  AZ_NODISCARD az_iot_result az_iot_sas_token_from_signature(
      const char* resource_uri,
      const char* key_name,
      uint64_t expiry_unix_seconds,
      const uint8_t signature[AZ_IOT_SHA256_SIZE],
      char* out,
      size_t out_size,
      size_t* out_len);

  /**
   * @brief Signs a token with @p key: az_iot_sas_token_string_to_sign(), its
   * HMAC-SHA256 with @p crypto, then az_iot_sas_token_from_signature().
   * Intermediate values are wiped.
   *
   * @param[in]  crypto               Crypto backend (SHA-256 required).
   * @param[in]  key                  Decoded device key.
   * @param[in]  key_len              Bytes in @p key; not 0.
   * @param[in]  resource_uri         As for az_iot_sas_token_from_signature().
   * @param[in]  key_name             As for az_iot_sas_token_from_signature().
   * @param[in]  expiry_unix_seconds  Expiry; not 0.
   * @param[out] out                  As for az_iot_sas_token_from_signature().
   * @param[in]  out_size             Bytes in @p out.
   * @param[out] out_len              Characters written, terminator excluded.
   * @return As az_iot_sas_token_from_signature(); AZ_IOT_ERR_INVALID_ARG also
   * for an unusable @p crypto or an empty key; the backend's error otherwise.
   */
  AZ_NODISCARD az_iot_result az_iot_sas_token_sign(
      const az_iot_crypto* crypto,
      const uint8_t* key,
      size_t key_len,
      const char* resource_uri,
      const char* key_name,
      uint64_t expiry_unix_seconds,
      char* out,
      size_t out_size,
      size_t* out_len);

  /**
   * @brief Derives a device key from an enrollment-group key:
   * HMAC-SHA256(group key, @p id), as az_iot_auth::sas::is_enrollment_group_key.
   *
   * @param[in]  crypto         Crypto backend (SHA-256 required).
   * @param[in]  group_key      Decoded group key.
   * @param[in]  group_key_len  Bytes in @p group_key; not 0.
   * @param[in]  id             DPS registration ID (device ID for a direct hub
   *                            connection); not empty.
   * @param[out] out            Device key; zeroed on failure.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for a NULL pointer, an empty key
   * or ID, or an unusable @p crypto; the backend's error otherwise.
   */
  AZ_NODISCARD az_iot_result az_iot_sas_derive_device_key(
      const az_iot_crypto* crypto,
      const uint8_t* group_key,
      size_t group_key_len,
      const char* id,
      uint8_t out[AZ_IOT_SHA256_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SAS_TOKEN_H */
