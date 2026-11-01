// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/** @file e2e_mqtt_adapter.h
 * @brief The MQTT adapter under test (AZ_IOT_E2E_ADAPTER_PAHO or AZ_IOT_E2E_ADAPTER_AZ_MQTT),
 * building a connection client's MQTT clients in a heap workspace.
 */
#ifndef E2E_MQTT_ADAPTER_H
#define E2E_MQTT_ADAPTER_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#if defined(AZ_IOT_E2E_ADAPTER_PAHO)
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#elif defined(AZ_IOT_E2E_ADAPTER_AZ_MQTT)
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#else
#error "Define AZ_IOT_E2E_ADAPTER_PAHO or AZ_IOT_E2E_ADAPTER_AZ_MQTT"
#endif

/** @brief Largest outgoing packet: 1 MiB + 64 KiB. The mqttv5 telemetry size cases publish up to
 * 1 MiB + 4 KiB, which with its MQTT headers must still fit, so that the hub's limit is what they
 * test, not the client's. */
#define E2E_MQTT_SEND_BUFFER_SIZE (1024u * 1024u + 64u * 1024u)

/**
 * @brief Has @p opts build its MQTT clients with the adapter under test, in a heap workspace.
 *
 * @return The workspace, to free() after az_iot_connection_client_deinit(); NULL when out of memory
 * (@p opts unchanged).
 */
static inline uint8_t* e2e_use_mqtt_adapter(az_iot_connection_client_options* opts)
{
  /* Receive: the IoT Hub limits (az_mqtt's default footprint). The store holds a resend of the
   * largest packet, for the hub session that is resumed. Connect strings: the TLS PEM material and
   * SAS tokens. Paho sizes its own buffers and only checks these. */
  static const az_iot_mqtt_limits k_limits = {
    .send_buffer_size = E2E_MQTT_SEND_BUFFER_SIZE,
    .receive_buffer_size = 264u * 1024u,
    .max_in_flight_messages = 64,
    .message_store_size = E2E_MQTT_SEND_BUFFER_SIZE + 1024u,
    .connect_strings_size = 64u * 1024u,
  };
#if defined(AZ_IOT_E2E_ADAPTER_PAHO)
  const az_iot_mqtt_adapter* adapter = az_iot_paho_get_adapter();
#else
  const az_iot_mqtt_adapter* adapter = az_iot_az_mqtt_get_adapter();
#endif
  size_t const size = adapter->get_workspace_size(&k_limits);
  uint8_t* workspace = size > 0 ? (uint8_t*)malloc(size) : NULL;
  if (workspace != NULL)
  {
    opts->mqtt_adapter = adapter;
    opts->mqtt_limits = k_limits;
    opts->mqtt_workspace = workspace;
    opts->mqtt_workspace_size = size;
  }
  return workspace;
}

#endif /* E2E_MQTT_ADAPTER_H */
