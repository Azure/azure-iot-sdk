// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* mbedTLS-backed ADU crypto primitives (RSASSA-PKCS1-v1_5 over SHA-256 verify +
 * one-shot/incremental SHA-256). Mirrors adapters/adu/crypto_openssl but uses
 * the mbedTLS that ships with ESP-IDF, so the ESP32 build pulls in no extra
 * crypto library. All JWS/SJWK orchestration stays in ADU core. */
#ifndef AZ_IOT_ADU_CRYPTO_MBEDTLS_H
#define AZ_IOT_ADU_CRYPTO_MBEDTLS_H

#include "azure/iot/az_iot_adu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Return az_iot_adu_crypto_hooks_t wired to the mbedTLS backend. The struct is
 * by value, references static function pointers, and carries no allocated state
 * (user_ctx is NULL). Safe to pass straight to az_iot_adu_client_initialize(). */
az_iot_adu_crypto_hooks_t az_iot_adu_crypto_mbedtls_hooks(void);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADU_CRYPTO_MBEDTLS_H */
