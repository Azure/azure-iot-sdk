// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_crypto_openssl.h
 * @brief az_iot_crypto backend on OpenSSL 3.0+.
 */
#ifndef AZ_IOT_CRYPTO_OPENSSL_H
#define AZ_IOT_CRYPTO_OPENSSL_H

#include "azure/iot/az_iot_crypto.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Returns the OpenSSL backend, with every function including
   * verify_rs256. Static storage; stateless.
   *
   * @return The backend.
   */
  const az_iot_crypto* az_iot_crypto_openssl(void);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CRYPTO_OPENSSL_H */
