// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_MQTTV5_TELEMETRY_CLIENT_H
#define AZ_IOT_MQTTV5_TELEMETRY_CLIENT_H

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct az_iot_mqttv5_telemetry_client
  {
    struct
    {
      az_iot_connection_client* conn;
    } _internal;
  } az_iot_mqttv5_telemetry_client;

  /* Records that the connection must resolve to the MQTT v5 profile; it need
   * not be open yet. A connection already known to be MQTTv3, or one an mqttv3
   * client is already attached to, is rejected with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH. Otherwise a conflict fails the
   * connection when the profile resolves, before it reports CONNECTED. */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_telemetry_client_init(
      az_iot_mqttv5_telemetry_client* client,
      az_iot_connection_client* conn);

  void az_iot_mqttv5_telemetry_client_deinit(az_iot_mqttv5_telemetry_client* client);

  /**
   * @brief Send @p message as QoS 1 telemetry.
   *
   * QoS is always 1; for QoS 0, publish on a custom topic (az_iot_custom_topic_client.h). Nothing
   * is queued: a send while not connected fails, and an accepted send cannot be cancelled. The
   * callback is the only completion signal; the SDK exposes no pending-send count, packet id or
   * raw reason code.
   *
   * The #AZ_IOT_MSG_PROP_CONTENT_TYPE and #AZ_IOT_MSG_PROP_CORRELATION_ID properties travel as
   * the native MQTT v5 Content Type and Correlation Data (UTF-8 text, at most 65535 bytes); the
   * hub reads the correlation id only from there. Every other property with a non-empty key
   * travels, in order, as an MQTT v5 User Property, after the `type` and `content-type` ones the
   * client adds; a NULL value is sent empty. There is no limit on their number other than what the
   * MQTT adapter can send in one packet. No Message Expiry Interval is sent: the hub
   * routes telemetry on arrival and does not carry it.
   *
   * @param callback Required. Called once, only if this returns AZ_IOT_OK: with the broker's
   *                 acknowledgement result, or with AZ_IOT_ERR_NOT_CONNECTED if the session ends
   *                 first (also on az_iot_connection_client_close()); the message may then not
   *                 have arrived, so resend if needed. AZ_IOT_ERR_PUBLISH_REFUSED: refused, and
   *                 will be again if resent unchanged. AZ_IOT_ERR_BUSY: over the hub's quota.
   *                 AZ_IOT_ERR_MQTT: any other refusal.
   *                 Still called after az_iot_mqttv5_telemetry_client_deinit(); not called after
   *                 az_iot_connection_client_deinit().
   *
   * @return AZ_IOT_OK if handed to the transport.
   * @return AZ_IOT_ERR_INVALID_ARG if @p callback is NULL, among other arguments, or the
   *         correlation id is longer than 65535 bytes.
   * @return AZ_IOT_ERR_NOT_CONNECTED if the connection is not connected, or its session is being
   *         ended on purpose and its DISCONNECT is sent; nothing was sent.
   * @return AZ_IOT_ERR_BUSY if the limits.max_pending_publishes slots not reserved by other
   *         feature clients are all awaiting acknowledgement, or a SAS renewal is letting the
   *         session's deliveries settle before its DISCONNECT (docs/connecting.md); nothing was
   *         sent, retry later.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_telemetry_client_send(
      az_iot_mqttv5_telemetry_client* client,
      const az_iot_telemetry_message* message,
      az_iot_telemetry_send_callback callback,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV5_TELEMETRY_CLIENT_H */
