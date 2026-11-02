// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_adapter_paho.h
 * @brief Public entry points for the Paho-C MQTT adapter.
 *
 * Set az_iot_connection_client_options::mqtt_adapter to az_iot_paho_get_adapter(). The
 * connection client builds one Paho client per session in mqtt_workspace, of the MQTT version
 * the session needs (v3.1.1 for DPS and mqttv3, v5 for mqttv5).
 *
 * @code
 * static uint8_t mqtt_workspace[AZ_IOT_PAHO_WORKSPACE_SIZE];
 * opts.mqtt_adapter = az_iot_paho_get_adapter();
 * opts.mqtt_limits = (az_iot_mqtt_limits){ .send_buffer_size = 1, .receive_buffer_size = 1,
 *                                          .max_in_flight_messages = 1 };
 * opts.mqtt_workspace = mqtt_workspace;
 * opts.mqtt_workspace_size = sizeof(mqtt_workspace);
 * @endcode
 */
#ifndef AZ_IOT_ADAPTER_PAHO_H
#define AZ_IOT_ADAPTER_PAHO_H

#include "../az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief The Paho adapter for clients in application memory (a constant). The workspace holds
   * the adapter's own state; Paho still allocates its internals.
   */
  const az_iot_mqtt_adapter* az_iot_paho_get_adapter(void);

  /**
   * @brief Workspace bytes for one Paho client; 0 when @p limits is invalid. Paho sizes its own
   * buffers: the values of @p limits are only checked.
   */
  size_t az_iot_paho_get_workspace_size(const az_iot_mqtt_limits* limits);

/** @brief Upper bound of az_iot_paho_get_workspace_size(), for static arrays; checked by the
 * adapter at compile time. */
#define AZ_IOT_PAHO_WORKSPACE_SIZE 512u

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADAPTER_PAHO_H */
