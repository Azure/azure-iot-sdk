// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* OpenSSL-backed crypto primitives for the ADU client (Phase 2).
 *
 * This adapter provides ONLY the cryptographic primitives the ADU core needs
 * (RSASSA-PKCS1-v1_5 over SHA-256 verification, and one-shot + incremental
 * SHA-256). All JWS/SJWK parsing, base64url decoding, root-key resolution and
 * revocation policy live in ADU core (see docs/azure-device-update.md section 6),
 * so this adapter takes no key material and holds no ADU state. */
#ifndef AZ_IOT_ADU_CRYPTO_OPENSSL_H
#define AZ_IOT_ADU_CRYPTO_OPENSSL_H

#include "azure/iot/az_iot_adu.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Return an az_iot_adu_crypto_hooks wired to the OpenSSL (3.0+) backend.
 * The returned struct is by value; it references static function pointers and
 * carries no allocated state (user_ctx is NULL). Safe to pass directly to
 * az_iot_adu_client_initialize().
 */
az_iot_adu_crypto_hooks az_iot_adu_crypto_openssl_hooks(void);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADU_CRYPTO_OPENSSL_H */
