// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Test certificate provider, for suites that drive a connection client over a
 * mock adapter or a local test broker. */
#ifndef AZ_IOT_TEST_PROVIDER_H
#define AZ_IOT_TEST_PROVIDER_H

#include "azure/iot/az_iot_certificate_provider.h"
#include "azure/iot/az_iot_connection_client.h"

/**
 * @brief Certificate provider whose load() returns, for every role, material
 * with no client identity: server-authentication-only TLS.
 */
typedef struct az_iot_test_provider
{
  az_iot_certificate_provider base;
  /** CA file the client trusts; NULL for none. Not copied. */
  const char* trusted_ca_path;
} az_iot_test_provider;

/**
 * @brief Initialize @p provider to trust @p trusted_ca_path (may be NULL).
 */
void az_iot_test_provider_init(az_iot_test_provider* provider, const char* trusted_ca_path);

/**
 * @brief az_iot_connection_client_init() with a shared az_iot_test_provider
 * (no CA) when @p opts has no certificate_provider.
 */
AZ_NODISCARD az_iot_result az_iot_test_connection_client_init(
    az_iot_connection_client* client,
    const az_iot_connection_client_options* opts);

#endif /* AZ_IOT_TEST_PROVIDER_H */
