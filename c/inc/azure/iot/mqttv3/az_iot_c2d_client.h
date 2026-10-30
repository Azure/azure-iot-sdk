// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_MQTTV3_C2D_CLIENT_H
#define AZ_IOT_MQTTV3_C2D_CLIENT_H

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct az_iot_mqttv3_c2d_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_c2d_handler_callback handler;
      void* handler_ctx;
    } _internal;
  } az_iot_mqttv3_c2d_client;

  /**
   * @brief Initialize the MQTTv3 hub C2D client.
   *
   * Subscribes `devices/{device_id}/messages/devicebound/#` and decodes the
   * topic property bag on each delivery.
   *
   * The connection need not be open: this records that it must resolve to the
   * MQTTv3 profile, and the topics are built when it connects and the assigned
   * device id is known. A connection already known to be MQTT_V5 -- a direct
   * connection, or a DPS one past assignment -- is rejected here with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH; otherwise a conflict surfaces when
   * the connection resolves, which fails it before it reports CONNECTED. The
   * same error is returned when an mqttv5 client is already attached.
   */
  AZ_NODISCARD az_iot_result
  az_iot_mqttv3_c2d_client_init(az_iot_mqttv3_c2d_client* client, az_iot_connection_client* conn);

  void az_iot_mqttv3_c2d_client_deinit(az_iot_mqttv3_c2d_client* client);

  /** NULL @p cb pauses delivery without tearing the subscription down. */
  az_iot_result az_iot_mqttv3_c2d_client_set_handler(
      az_iot_mqttv3_c2d_client* client,
      az_iot_c2d_handler_callback cb,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV3_C2D_CLIENT_H */
