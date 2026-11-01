// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file
 * @brief Publish to the custom topics configured on an mqttv5 IoT hub.
 *
 * The hub's topic templates decide which topics a device may publish to; the
 * device publishes concrete topics, with its own device id where a template
 * has `{deviceId}`. Publish only: custom topics cannot be subscribed to.
 */
#ifndef AZ_IOT_MQTTV5_CUSTOM_TOPIC_CLIENT_H
#define AZ_IOT_MQTTV5_CUSTOM_TOPIC_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** @brief Longest topic accepted, in bytes, excluding the terminator. The hub's limit. */
#define AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN 256

/** @brief Most `/`-separated levels a topic may have. The hub's limit. */
#define AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEVELS 15

/** @brief The template variable az_iot_mqttv5_custom_topic_client_format_topic() expands. */
#define AZ_IOT_MQTTV5_CUSTOM_TOPIC_DEVICE_ID_VARIABLE "{deviceId}"

  /**
   * @brief A message for a custom topic. Zero-initialize, then set what you need.
   *
   * Sent as given: the client adds no user properties of its own.
   */
  typedef struct az_iot_mqttv5_custom_topic_message
  {
    /** Concrete topic: UTF-8, no `+` or `#`, first level not `ih`. Sent as given: `{deviceId}` is
     * not expanded here; see az_iot_mqttv5_custom_topic_client_format_topic(). */
    const char* topic;
    const uint8_t* payload; /**< May be NULL when @p payload_len is 0. */
    size_t payload_len;
    az_iot_mqtt_qos qos; /**< AZ_IOT_MQTT_QOS_0 (default) or AZ_IOT_MQTT_QOS_1. Refusals are
                              reported at QoS 1 only. */
    const char* content_type; /**< MQTT v5 Content Type. NULL: none. */
    /** Sent verbatim. Keys must be non-empty; values non-NULL. */
    const az_iot_mqtt_user_property* user_properties;
    size_t user_properties_count;
    uint32_t message_expiry_seconds; /**< 0: none. */
  } az_iot_mqttv5_custom_topic_message;

  /**
   * @brief Completion of az_iot_mqttv5_custom_topic_client_publish().
   *
   * @param result   Outcome, with the hub's reason code and Reason String when it refused the
   *                 message; see az_iot_mqttv5_custom_topic_client_publish(). Valid only during
   *                 the call.
   * @param user_ctx Context passed to az_iot_mqttv5_custom_topic_client_publish().
   */
  typedef void (*az_iot_mqttv5_custom_topic_publish_callback)(
      const az_iot_publish_result* result,
      void* user_ctx);

  /** @brief Custom topic client. Caller-allocated; opaque. */
  typedef struct az_iot_mqttv5_custom_topic_client
  {
    struct
    {
      az_iot_connection_client* conn;
    } _internal;
  } az_iot_mqttv5_custom_topic_client;

  /**
   * @brief Attach to @p conn, which need not be open yet.
   *
   * @param client Not initialized, or deinitialized: initializing a live instance again leaks its
   *               hold on the mqttv5 profile.
   * @param conn   Must outlive @p client.
   *
   * @return AZ_IOT_OK on success.
   * @retval AZ_IOT_ERR_INVALID_ARG @p client or @p conn is NULL.
   * @retval AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH @p conn is already known to be mqttv3, or an
   *         mqttv3 client is attached to it. Otherwise a conflict fails the connection when its
   *         profile resolves.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_custom_topic_client_init(
      az_iot_mqttv5_custom_topic_client* client,
      az_iot_connection_client* conn);

  /** @brief Detach. NULL and repeated calls are ignored. */
  void az_iot_mqttv5_custom_topic_client_deinit(az_iot_mqttv5_custom_topic_client* client);

  /**
   * @brief Build a topic from @p topic_template, replacing each `{deviceId}` with this device's id.
   *
   * The variable is case-sensitive, as on the hub. The device id can change when the device is
   * provisioned again, so build the topic for each publish rather than once: a topic built earlier
   * keeps the old id, and az_iot_mqttv5_custom_topic_client_publish() sends it unchanged. A device
   * id with `+` or `#` yields a topic that publish() rejects: MQTT forbids them in topic names.
   *
   * @param[out] out_len Optional. Length written, excluding the terminator.
   *
   * @return AZ_IOT_OK on success; @p out holds a NUL-terminated topic.
   * @retval AZ_IOT_ERR_INVALID_ARG A NULL argument, or a `{` or `}` that is not part of
   *         `{deviceId}`.
   * @retval AZ_IOT_ERR_NOT_CONNECTED The connection is not connected, so the device id is not
   *         final.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE @p out_size is too small.
   * @retval AZ_IOT_ERR_NOT_SUPPORTED @p client is not initialized.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_custom_topic_client_format_topic(
      const az_iot_mqttv5_custom_topic_client* client,
      const char* topic_template,
      char* out,
      size_t out_size,
      size_t* out_len);

  /**
   * @brief Publish @p message.
   *
   * @param callback Required. Called once, only if this returns AZ_IOT_OK; result->status is:
   *                 - QoS 0: AZ_IOT_OK, before this returns. The hub does not acknowledge QoS 0,
   *                   so a message it refuses (for example, a topic no template allows) is
   *                   dropped without any error reaching the device. Use QoS 1 to learn of
   *                   refusals.
   *                 - QoS 1: the hub's verdict. AZ_IOT_OK: accepted.
   *                   AZ_IOT_ERR_PUBLISH_REFUSED: refused, for example a topic no template allows;
   *                   it will be again if resent unchanged. AZ_IOT_ERR_BUSY: over the hub's quota.
   *                   AZ_IOT_ERR_MQTT: failed for another reason. AZ_IOT_ERR_NOT_CONNECTED: the
   *                   session ended first; the message may not have arrived. On a refusal,
   *                   result->reason_code and result->reason_string say why.
   *                 Not called after az_iot_connection_client_deinit().
   *
   * @return AZ_IOT_OK if handed to the transport.
   * @retval AZ_IOT_ERR_INVALID_ARG A NULL argument, an invalid topic (see
   *         az_iot_mqttv5_custom_topic_message::topic, AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN,
   *         AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEVELS), a QoS other than 0 or 1, a count without
   *         its array, or a user property with an empty key or NULL value. Nothing was sent.
   * @retval AZ_IOT_ERR_NOT_SUPPORTED @p client is not initialized.
   * @retval AZ_IOT_ERR_NOT_CONNECTED The connection is not connected, or its session is being ended
   *         on purpose and its DISCONNECT is sent.
   * @retval AZ_IOT_ERR_BUSY QoS 1, and every pending-acknowledgement slot it may use is taken, or a
   *         SAS renewal is letting the session's deliveries settle before its DISCONNECT (QoS 0
   *         is still sent; docs/connecting.md); nothing was sent, retry later.
   */
  AZ_NODISCARD az_iot_result az_iot_mqttv5_custom_topic_client_publish(
      az_iot_mqttv5_custom_topic_client* client,
      const az_iot_mqttv5_custom_topic_message* message,
      az_iot_mqttv5_custom_topic_publish_callback callback,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTTV5_CUSTOM_TOPIC_CLIENT_H */
