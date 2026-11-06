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
 * (no CA) when @p opts has no certificate_provider, a default-size
 * workspace lent to @p client when @p opts has no workspace, the mock MQTT
 * adapter (az_iot_mock_mqtt_get_adapter()) when @p opts has no mqtt_adapter,
 * and, when @p opts has no mqtt_workspace, an mqtt_workspace sized for its
 * mqtt_limits (az_iot_mock_mqtt_limits when all are 0) lent too, until
 * az_iot_test_connection_client_deinit().
 */
AZ_NODISCARD az_iot_result az_iot_test_connection_client_init(
    az_iot_connection_client* client,
    const az_iot_connection_client_options* opts);

/**
 * @brief Has @p opts build its MQTT clients with the mock adapter, in one MQTT
 * workspace every caller shares: for connections never opened, or opened one
 * at a time. Leaves the certificate provider and workspace as they are.
 */
void az_iot_test_use_mock_mqtt_adapter(az_iot_connection_client_options* opts);

/**
 * @brief az_iot_connection_client_deinit(), then frees the workspace
 * az_iot_test_connection_client_init() lent @p client, if any, when it succeeded.
 *
 * @return az_iot_connection_client_deinit()'s result.
 */
az_iot_result az_iot_test_connection_client_deinit(az_iot_connection_client* client);

#endif /* AZ_IOT_TEST_PROVIDER_H */
