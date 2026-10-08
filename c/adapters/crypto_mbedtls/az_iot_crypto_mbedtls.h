// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_crypto_mbedtls.h
 * @brief az_iot_crypto backend on mbedTLS PSA Crypto (3.6 LTS or 4.1+),
 * e.g. the mbedTLS shipped with ESP-IDF.
 */
#ifndef AZ_IOT_CRYPTO_MBEDTLS_H
#define AZ_IOT_CRYPTO_MBEDTLS_H

#include "azure/iot/az_iot_crypto.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Returns the mbedTLS backend, with every function including
   * verify_rs256. Static storage; stateless; no heap use for SHA-256.
   *
   * @return The backend.
   */
  const az_iot_crypto* az_iot_crypto_mbedtls(void);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CRYPTO_MBEDTLS_H */
