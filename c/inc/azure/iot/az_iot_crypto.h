// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_crypto.h
 * @brief Cryptographic backend: SHA-256 and RS256 verification.
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
   * whose state does not fit keeps a pointer here.
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
     * On failure the stream is unchanged; @p ctx must still be finalized.
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

    /**
     * @brief Verifies an RSASSA-PKCS1-v1_5 SHA-256 signature (JWS RS256).
     * Optional; software updates require it.
     *
     * @param[in] self          Backend.
     * @param[in] modulus       RSA modulus, big-endian.
     * @param[in] modulus_len   Bytes in @p modulus.
     * @param[in] exponent      RSA public exponent, big-endian.
     * @param[in] exponent_len  Bytes in @p exponent.
     * @param[in] data          Signed data.
     * @param[in] data_len      Bytes in @p data.
     * @param[in] signature     Signature.
     * @param[in] signature_len Bytes in @p signature.
     * @return AZ_IOT_OK only for a valid signature.
     */
    az_iot_result (*verify_rs256)(
        const az_iot_crypto* self,
        const uint8_t* modulus,
        size_t modulus_len,
        const uint8_t* exponent,
        size_t exponent_len,
        const uint8_t* data,
        size_t data_len,
        const uint8_t* signature,
        size_t signature_len);
  };

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CRYPTO_H */
