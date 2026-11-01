// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file install_test_mqtt_adapter.h
 * @brief A stub MQTT adapter, so a connection client can be initialized while
 * the test links only the component under test.
 *
 * init() of the connection client needs an adapter but builds no MQTT client
 * until open(), so the stub's init() is never reached.
 */
#ifndef AZ_IOT_INSTALL_TEST_MQTT_ADAPTER_H
#define AZ_IOT_INSTALL_TEST_MQTT_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"

/** @brief Bytes the stub asks for, whatever the limits. */
#define INSTALL_TEST_MQTT_WORKSPACE_SIZE 64u

/** @brief The stub's workspace size: a fixed size for any limits. */
static size_t install_test_mqtt_get_workspace_size(const az_iot_mqtt_limits* limits)
{
  (void)limits;
  return INSTALL_TEST_MQTT_WORKSPACE_SIZE;
}

/** @brief Refuses: the installed-package tests never open a session. */
static az_iot_result install_test_mqtt_init(
    az_iot_mqtt_version mqtt_version,
    const az_iot_mqtt_limits* limits,
    uint8_t* workspace,
    size_t workspace_size,
    az_iot_mqtt_client** out_client)
{
  (void)mqtt_version;
  (void)limits;
  (void)workspace;
  (void)workspace_size;
  (void)out_client;
  return AZ_IOT_ERR_NOT_SUPPORTED;
}

/** @brief The stub adapter. */
static const az_iot_mqtt_adapter install_test_mqtt_adapter
    = { install_test_mqtt_get_workspace_size, install_test_mqtt_init };

/** @brief Workspace for the stub adapter. */
static uint8_t install_test_mqtt_workspace[INSTALL_TEST_MQTT_WORKSPACE_SIZE];

/** @brief Sets @p options' MQTT adapter, valid limits and workspace to the stub's. */
static void install_test_use_stub_mqtt_adapter(az_iot_connection_client_options* options)
{
  options->mqtt_adapter = &install_test_mqtt_adapter;
  options->mqtt_limits.send_buffer_size = 1024u;
  options->mqtt_limits.receive_buffer_size = 1024u;
  options->mqtt_limits.max_in_flight_messages = 1u;
  options->mqtt_limits.message_store_size = 0u;
  options->mqtt_limits.connect_strings_size = 1024u;
  options->mqtt_workspace = install_test_mqtt_workspace;
  options->mqtt_workspace_size = sizeof(install_test_mqtt_workspace);
}

#endif /* AZ_IOT_INSTALL_TEST_MQTT_ADAPTER_H */
