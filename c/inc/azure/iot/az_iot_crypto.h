// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_crypto.h
 * @brief Cryptographic backend: SHA-256.
 *
 * Set once on the connection client (az_iot_connection_client_options::crypto);
 * every feature that needs crypto uses it. Shipped backends:
 * az_iot_crypto_openssl() and az_iot_crypto_mbedtls(). To bring your own, fill
 * an az_iot_crypto with your functions; embed it as the first member of a
 * larger struct to reach backend state from @c self.
 */
#ifndef AZ_IOT_CRYPTO_H
#define AZ_IOT_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#include "az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Value of az_iot_crypto::version this header defines. */
#define AZ_IOT_CRYPTO_VERSION 1u

/** @brief SHA-256 digest size, in bytes. */
#define AZ_IOT_SHA256_SIZE 32

  /**
   * @brief Caller-owned SHA-256 state. Layout is backend-defined; a backend
   * whose state does not fit keeps a pointer here. Must not be copied between
   * sha256_init() and sha256_final().
   */
  typedef struct az_iot_sha256_ctx
  {
    uint64_t opaque[64]; /**< Backend state. */
  } az_iot_sha256_ctx;

  typedef struct az_iot_crypto az_iot_crypto;

  /**
   * @brief Cryptographic backend. Every function returns AZ_IOT_OK on success.
   */
  struct az_iot_crypto
  {
    /** @brief Must be AZ_IOT_CRYPTO_VERSION. */
    uint32_t version;

    /**
     * @brief Starts a SHA-256 computation in @p ctx. Required.
     *
     * On failure @p ctx holds nothing to release.
     */
    az_iot_result (*sha256_init)(const az_iot_crypto* self, az_iot_sha256_ctx* ctx);

    /**
     * @brief Hashes @p len bytes of @p data. @p data may be NULL when @p len
     * is 0. Required.
     *
     * Rejected arguments (AZ_IOT_ERR_INVALID_ARG) leave the stream unchanged;
     * after any other failure the digest is unusable. Either way @p ctx must
     * still be passed to sha256_final().
     */
    az_iot_result (*sha256_update)(
        const az_iot_crypto* self,
        az_iot_sha256_ctx* ctx,
        const uint8_t* data,
        size_t len);

    /**
     * @brief Writes the digest to @p out and releases @p ctx, also on
     * failure. @p out NULL only releases @p ctx. Required.
     */
    az_iot_result (*sha256_final)(
        const az_iot_crypto* self,
        az_iot_sha256_ctx* ctx,
        uint8_t out[AZ_IOT_SHA256_SIZE]);
  };

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CRYPTO_H */
