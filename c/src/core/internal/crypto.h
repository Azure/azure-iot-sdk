// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file crypto.h
 * @brief Primitives the SDK composes from an az_iot_crypto backend.
 */
#ifndef AZ_IOT_CRYPTO_INTERNAL_H
#define AZ_IOT_CRYPTO_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_crypto.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Checks that @p crypto is a usable backend.
   *
   * @param[in] crypto Backend.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG when @p crypto is NULL, has
   * another version, or lacks a SHA-256 function.
   */
  AZ_NODISCARD az_iot_result az_iot_crypto__validate(const az_iot_crypto* crypto);

  /**
   * @brief One-shot SHA-256.
   *
   * @param[in] crypto   Validated backend.
   * @param[in] data     Input; may be NULL when @p data_len is 0.
   * @param[in] data_len Bytes in @p data.
   * @param[out] out     Digest.
   * @return AZ_IOT_OK, or the backend's error.
   */
  AZ_NODISCARD az_iot_result az_iot_crypto__sha256(
      const az_iot_crypto* crypto,
      const uint8_t* data,
      size_t data_len,
      uint8_t out[AZ_IOT_SHA256_SIZE]);

  /**
   * @brief HMAC-SHA256 (RFC 2104) over the backend's SHA-256.
   *
   * @param[in] crypto   Validated backend.
   * @param[in] key      Key; may be NULL when @p key_len is 0.
   * @param[in] key_len  Bytes in @p key.
   * @param[in] data     Input; may be NULL when @p data_len is 0.
   * @param[in] data_len Bytes in @p data.
   * @param[out] out     MAC.
   * @return AZ_IOT_OK, or the backend's error.
   */
  AZ_NODISCARD az_iot_result az_iot_crypto__hmac_sha256(
      const az_iot_crypto* crypto,
      const uint8_t* key,
      size_t key_len,
      const uint8_t* data,
      size_t data_len,
      uint8_t out[AZ_IOT_SHA256_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CRYPTO_INTERNAL_H */
