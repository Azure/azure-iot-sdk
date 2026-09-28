// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Test-only init for suites that drive a connection client over a mock adapter
 * or a plaintext test broker, without a certificate provider. */
#ifndef AZ_IOT_TEST_PLAINTEXT_CLIENT_H
#define AZ_IOT_TEST_PLAINTEXT_CLIENT_H

#include "azure/iot/az_iot_connection_client.h"

/**
 * @brief az_iot_connection_client_init(), then, on success, allow the client to
 * connect without a certificate provider (plaintext). Test code only.
 */
az_iot_result az_iot_test_connection_client_init(
    az_iot_connection_client* client,
    const az_iot_connection_client_options* opts);

#endif /* AZ_IOT_TEST_PLAINTEXT_CLIENT_H */
