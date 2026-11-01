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
   * QoS is always 1. Nothing is queued: a send while not connected fails, and an accepted send
   * cannot be cancelled. The callback is the only completion signal; the SDK exposes no
   * pending-send count or packet id.
   *
   * @param callback Required. Called once, only if this returns AZ_IOT_OK: with the broker's
   *                 acknowledgement result, or with AZ_IOT_ERR_NOT_CONNECTED if the session ends
   *                 first (also on az_iot_connection_client_close()); the message may then not
   *                 have arrived, so resend if needed. Still called after
   *                 az_iot_mqttv3_telemetry_client_deinit(); not called after
   *                 az_iot_connection_client_deinit().
   *
   * @return AZ_IOT_OK if handed to the transport.
   * @return AZ_IOT_ERR_INVALID_ARG if @p callback is NULL, among other arguments.
   * @return AZ_IOT_ERR_NOT_CONNECTED if the connection is not connected, or its session is being
   *         ended on purpose (a SAS token renewal, a pause for a provisioning session, a failure)
   *         and its DISCONNECT is sent; nothing was sent.
   * @return AZ_IOT_ERR_BUSY if the limits.max_pending_publishes slots not reserved by other
   *         feature clients are all awaiting acknowledgement, or a SAS token renewal or a pause for
   *         a provisioning session is letting the session's deliveries settle before its
   *         DISCONNECT (at most 5 s; docs/connecting.md); nothing was sent, retry later.
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
