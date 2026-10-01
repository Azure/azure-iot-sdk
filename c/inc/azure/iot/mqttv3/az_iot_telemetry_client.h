// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_MQTTV3_TELEMETRY_CLIENT_H
#define AZ_IOT_MQTTV3_TELEMETRY_CLIENT_H

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct az_iot_mqttv3_telemetry_client
  {
    struct
    {
      az_iot_connection_client* conn;
    } _internal;
  } az_iot_mqttv3_telemetry_client;

  /* Records that the connection must resolve to the MQTTv3 profile; it need
   * not be open yet. A connection already known to be MQTT_V5, or one an mqttv5
   * client is already attached to, is rejected with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH. Otherwise a conflict fails the
   * connection when the profile resolves, before it reports CONNECTED. */
  AZ_NODISCARD az_iot_result az_iot_mqttv3_telemetry_client_init(
      az_iot_mqttv3_telemetry_client* client,
      az_iot_connection_client* conn);

  void az_iot_mqttv3_telemetry_client_deinit(az_iot_mqttv3_telemetry_client* client);

  /**
   * @brief Send @p message as QoS 1 telemetry.
   *
   * @p callback, if set, gets the broker's acknowledgement from
   * az_iot_connection_client_do_work().
   *
   * @return AZ_IOT_OK if handed to the transport.
   * @return AZ_IOT_ERR_BUSY if @p callback is set and AZ_IOT_MAX_PENDING_PUBACKS
   *         sends are awaiting acknowledgement; nothing was sent, retry later.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv3_telemetry_client_send(
      az_iot_mqttv3_telemetry_client* client,
      const az_iot_telemetry_message* message,
      az_iot_telemetry_send_callback callback,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV3_TELEMETRY_CLIENT_H */
